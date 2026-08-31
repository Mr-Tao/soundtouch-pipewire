default:
    @just --list

render-vault:
    #!/usr/bin/env sh
    : "${DEV_VAULT:?Set DEV_VAULT to the target Obsidian vault}"
    python3 tools/render_vault.py --vault "$DEV_VAULT"

render-vault-check:
    #!/usr/bin/env sh
    : "${DEV_VAULT:?Set DEV_VAULT to the target Obsidian vault}"
    python3 tools/render_vault.py --vault "$DEV_VAULT" --check

test-render-vault:
    python3 -B -m unittest discover -s tests -p 'test_render_vault.py'

vault-reviewed-manifest:
    python3 tools/render_vault.py --print-reviewed-manifest
