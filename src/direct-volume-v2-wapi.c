/* SPDX-License-Identifier: MIT */
#include "direct-volume-v2-wapi.h"

#include "direct-volume-v2.h"

typedef enum {
  OPERATION_NONE,
  OPERATION_GET,
  OPERATION_POST,
} Operation;

typedef struct {
  Operation operation;
  GWeakRef owner;
} Completion;

struct _StpwDirectVolumeV2WapiDriver {
  GObject parent_instance;
  StpwDirectVolumeV2 *core;
  StpwWapiClient *client;
  GCancellable *cancellable;
  Operation operation;
  StpwDirectVolumeV2WapiReportFunc report_func;
  gpointer report_data;
  GDestroyNotify report_destroy;
  gboolean disposed;
};

G_DEFINE_TYPE(StpwDirectVolumeV2WapiDriver, stpw_direct_volume_v2_wapi_driver,
              G_TYPE_OBJECT)

static void consume_effects(StpwDirectVolumeV2WapiDriver *self,
                            StpwDirectVolumeV2Effects effects);

static Completion *completion_new(StpwDirectVolumeV2WapiDriver *self,
                                  Operation operation) {
  Completion *completion = g_new0(Completion, 1);

  completion->operation = operation;
  g_weak_ref_init(&completion->owner, self);
  return completion;
}

static void completion_free(Completion *completion) {
  g_weak_ref_clear(&completion->owner);
  g_free(completion);
}

static StpwDirectVolumeV2PostResult classify_post_result(gboolean success,
                                                         const GError *error) {
  if (success)
    return STPW_DIRECT_VOLUME_V2_POST_DELIVERED;
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT))
    return STPW_DIRECT_VOLUME_V2_POST_NOT_DELIVERED;
  return STPW_DIRECT_VOLUME_V2_POST_POSSIBLY_DELIVERED;
}

static void operation_done(GObject *object, GAsyncResult *result,
                           gpointer user_data) {
  Completion *completion = user_data;
  g_autoptr(GError) error = NULL;
  StpwVolume volume = {0};
  gboolean success;
  StpwDirectVolumeV2WapiDriver *self;

  if (completion->operation == OPERATION_GET)
    success = stpw_wapi_get_volume_finish(STPW_WAPI_CLIENT(object), result,
                                          &volume, &error);
  else
    success =
        stpw_wapi_set_volume_finish(STPW_WAPI_CLIENT(object), result, &error);

  self = g_weak_ref_get(&completion->owner);
  if (self == NULL)
    goto out;
  if (self->disposed || self->operation != completion->operation)
    goto out_unref;

  self->operation = OPERATION_NONE;
  g_clear_object(&self->cancellable);

  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    goto out_unref;

  if (completion->operation == OPERATION_GET) {
    consume_effects(
        self, success
                  ? stpw_direct_volume_v2_observe(self->core, volume.actual,
                                                  volume.target, volume.muted)
                  : stpw_direct_volume_v2_get_failed(self->core));
  } else {
    consume_effects(self,
                    stpw_direct_volume_v2_post_complete(
                        self->core, classify_post_result(success, error)));
  }

out_unref:
  g_object_unref(self);
out:
  completion_free(completion);
}

static void start_action(StpwDirectVolumeV2WapiDriver *self,
                         StpwDirectVolumeV2Effects effects) {
  Completion *completion;

  if (self->disposed || effects.action == STPW_DIRECT_VOLUME_V2_ACTION_NONE)
    return;
  g_assert_cmpint(self->operation, ==, OPERATION_NONE);
  g_assert_null(self->cancellable);

  self->operation = effects.action == STPW_DIRECT_VOLUME_V2_ACTION_GET
                        ? OPERATION_GET
                        : OPERATION_POST;
  self->cancellable = g_cancellable_new();
  completion = completion_new(self, self->operation);

  if (self->operation == OPERATION_GET) {
    stpw_wapi_get_volume_async(self->client, self->cancellable, operation_done,
                               completion);
  } else {
    stpw_wapi_set_volume_async(self->client, effects.action_volume,
                               effects.action_muted, self->cancellable,
                               operation_done, completion);
  }
}

static void consume_effects(StpwDirectVolumeV2WapiDriver *self,
                            StpwDirectVolumeV2Effects effects) {
  start_action(self, effects);
  if ((effects.publish || effects.degraded) && self->report_func != NULL) {
    StpwDirectVolumeV2WapiReport report = {
        .publish = effects.publish,
        .volume = effects.published_volume,
        .muted = effects.published_muted,
        .degraded = effects.degraded,
    };

    self->report_func(self, &report, self->report_data);
  }
}

static void stpw_direct_volume_v2_wapi_driver_dispose(GObject *object) {
  StpwDirectVolumeV2WapiDriver *self =
      STPW_DIRECT_VOLUME_V2_WAPI_DRIVER(object);

  if (!self->disposed) {
    GDestroyNotify report_destroy = self->report_destroy;
    gpointer report_data = self->report_data;

    self->disposed = TRUE;
    self->report_func = NULL;
    self->report_data = NULL;
    self->report_destroy = NULL;
    if (self->cancellable != NULL)
      g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
    self->operation = OPERATION_NONE;
    g_clear_object(&self->client);
    if (report_destroy != NULL)
      report_destroy(report_data);
  }

  G_OBJECT_CLASS(stpw_direct_volume_v2_wapi_driver_parent_class)
      ->dispose(object);
}

static void stpw_direct_volume_v2_wapi_driver_finalize(GObject *object) {
  StpwDirectVolumeV2WapiDriver *self =
      STPW_DIRECT_VOLUME_V2_WAPI_DRIVER(object);

  stpw_direct_volume_v2_free(self->core);
  G_OBJECT_CLASS(stpw_direct_volume_v2_wapi_driver_parent_class)
      ->finalize(object);
}

static void stpw_direct_volume_v2_wapi_driver_class_init(
    StpwDirectVolumeV2WapiDriverClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = stpw_direct_volume_v2_wapi_driver_dispose;
  object_class->finalize = stpw_direct_volume_v2_wapi_driver_finalize;
}

static void
stpw_direct_volume_v2_wapi_driver_init(StpwDirectVolumeV2WapiDriver *self) {
  self->core = stpw_direct_volume_v2_new();
}

StpwDirectVolumeV2WapiDriver *stpw_direct_volume_v2_wapi_driver_new(
    StpwWapiClient *client, StpwDirectVolumeV2WapiReportFunc report_func,
    gpointer report_data, GDestroyNotify report_destroy) {
  StpwDirectVolumeV2WapiDriver *self;

  g_return_val_if_fail(STPW_IS_WAPI_CLIENT(client), NULL);
  self = g_object_new(STPW_TYPE_DIRECT_VOLUME_V2_WAPI_DRIVER, NULL);
  self->client = g_object_ref(client);
  self->report_func = report_func;
  self->report_data = report_data;
  self->report_destroy = report_destroy;
  return self;
}

StpwDirectVolumeV2WapiDriver *stpw_direct_volume_v2_wapi_driver_new_seeded(
    StpwWapiClient *client, const StpwVolume *initial,
    StpwDirectVolumeV2WapiReportFunc report_func, gpointer report_data,
    GDestroyNotify report_destroy) {
  StpwDirectVolumeV2WapiDriver *self;

  g_return_val_if_fail(STPW_IS_WAPI_CLIENT(client), NULL);
  g_return_val_if_fail(initial != NULL, NULL);
  g_return_val_if_fail(initial->target <= 100 && initial->actual <= 100, NULL);
  self = stpw_direct_volume_v2_wapi_driver_new(client, report_func, report_data,
                                               report_destroy);
  if (self == NULL)
    return NULL;
  (void)stpw_direct_volume_v2_observe(self->core, initial->actual,
                                      initial->target, initial->muted);
  return self;
}

gboolean
stpw_direct_volume_v2_wapi_driver_route(StpwDirectVolumeV2WapiDriver *self,
                                        gboolean have_volume, guint volume,
                                        gboolean have_mute, gboolean muted) {
  g_return_val_if_fail(STPW_IS_DIRECT_VOLUME_V2_WAPI_DRIVER(self), FALSE);
  if (!have_volume && !have_mute)
    return TRUE;
  if (have_volume && volume > 100)
    return FALSE;
  if (self->disposed)
    return TRUE;

  consume_effects(self, stpw_direct_volume_v2_route(self->core, have_volume,
                                                    volume, have_mute, muted));
  return TRUE;
}

void stpw_direct_volume_v2_wapi_driver_refresh(
    StpwDirectVolumeV2WapiDriver *self) {
  g_return_if_fail(STPW_IS_DIRECT_VOLUME_V2_WAPI_DRIVER(self));
  if (self->disposed)
    return;
  consume_effects(self, stpw_direct_volume_v2_refresh(self->core));
}

void stpw_direct_volume_v2_wapi_driver_invalidate(
    StpwDirectVolumeV2WapiDriver *self) {
  g_return_if_fail(STPW_IS_DIRECT_VOLUME_V2_WAPI_DRIVER(self));
  if (self->disposed)
    return;
  consume_effects(self, stpw_direct_volume_v2_invalidate(self->core));
}
