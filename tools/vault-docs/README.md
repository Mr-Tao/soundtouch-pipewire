# Obsidian export

Tyto české šablony jsou zdrojem stručné navigační vrstvy projektu v
`dev-vault`. Podrobným technickým zdrojem zůstávají commitnuté soubory
`README.md`, `docs/multiroom-control.md`, `docs/private-module-contract.md` a
`control/README.md`.

Export čte podrobné zdroje a šablony přes Git z `HEAD`, nikoli z pracovního
stromu. Všechny čtyři kanonické dokumenty, renderer, šablony a review manifest
musí být čisté a existovat v jediném checkoutnutém commitu. Tím se rozpracovaná
změna nemůže v Obsidianu tvářit jako popsaná či nasazená funkcionalita. Každý
výstup obsahuje přesné revize a otisky zdrojů, šablon, generátoru i manifestu.
Skutečně spuštěný renderer musí být byte-for-byte tímto commitnutým
generátorem; ani `--source` proto nemůže podvrhnout jinou provenance.

`reviewed-export.json` je review zámek. Váže společný otisk kanonických
dokumentů, společný otisk šablon a revizi i otisk generátoru. Po commitnutí
zkontrolovaných změn lze kandidátní obsah manifestu pouze vypsat příkazem
`just vault-reviewed-manifest`; po ručním review se manifest aktualizuje a
samostatně commitne. Render nezačne zapisovat, dokud není celý zámek platný.

```sh
DEV_VAULT=/cesta/k/dev-vault just render-vault
DEV_VAULT=/cesta/k/dev-vault just render-vault-check
just test-render-vault
just vault-reviewed-manifest
```

Cílový vault musí být zadán proměnnou `DEV_VAULT`; samotné `just` pouze vypíše
nabídku receptů. Renderer udržuje jen vlastní generované soubory a pojmenovaný
blok v `docs/index.md`; cizí části indexu neskenuje ani nepřepisuje. Do exportu
nepatří runtime snapshoty ani capture. Ty zůstávají vyloučené review workflow.
Renderer navíc fail-closed odmítá rozpoznané IPv4/IPv6 adresy, hardwarové
identifikátory, běžné tvary credentials a hlavičky privátních klíčů; tato
kontrola není obecná detekce libovolného capture či runtime obsahu.
Symlinky v cílových cestách jsou zakázané a content lease zabrání přepsání
souběžné cizí změny indexu. Jednotlivé soubory se zapisují přes atomický rename
s fsync souboru i rodičovského adresáře.
