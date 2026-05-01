# `luban doc`

Generate API documentation for the current project via Doxygen.

```text
luban doc [--open] [--clean]
```

luban does **not** bundle doxygen. Install once with:

```bat
luban setup --with doxygen
```

(`graphviz` is recommended too, for class/include graphs — `luban setup --with graphviz`.)

## What it does

1. If `Doxyfile` does not exist in the project root, materializes one
   from luban's bundled `Doxyfile.tmpl` template — pre-filled with the
   project name + version sniffed from `vcpkg.json` (or sensible defaults
   if vcpkg.json is absent). The Doxyfile is yours to edit; subsequent
   `luban doc` runs reuse it.
2. Runs `doxygen Doxyfile`, writing HTML to `build/doc/html/`.
3. Optionally opens `index.html` in your default browser (`--open`).

## Flags

| Flag | Effect |
|---|---|
| `--open` | After generation, open `build/doc/html/index.html` in the OS default browser. |
| `--clean` | Delete `build/doc/` and exit; no doxygen run. |

## Typical workflow

```bat
:: one-time tool install
luban setup --with doxygen --with graphviz

:: in a project
cd my-project
luban doc            :: writes build/doc/html/
luban doc --open     :: regenerate and pop the browser
luban doc --clean    :: wipe build/doc/
```

## Customizing the Doxyfile

luban's template is intentionally minimal — `INPUT = src include`, recursive,
HTML output to `build/doc/html`. Edit `Doxyfile` in the project root for
project-specific settings (`PROJECT_BRIEF`, `EXCLUDE_PATTERNS`, dot graphs,
themes, etc.). Re-running `luban doc` won't clobber your edits — the file
is materialized only on first run.

If you want to regenerate the template from scratch (e.g., to pick up an
upstream template change), delete `Doxyfile` and run `luban doc` again.

## Doxygen-awesome theme

luban's Doxyfile template references `doxygen-awesome.css` if present in the
project root. Drop in [doxygen-awesome-css](https://github.com/jothepro/doxygen-awesome-css)
for a modern look:

```bat
:: from project root, one-time
curl -fsSLO https://raw.githubusercontent.com/jothepro/doxygen-awesome-css/main/doxygen-awesome.css
luban doc --open
```

## CI integration

In `.github/workflows/docs.yml` (or equivalent), invoke directly — no need
for `luban doc` since CI may not have luban installed:

```yaml
- name: Install Doxygen
  run: sudo apt-get install -y doxygen graphviz
- name: Generate API docs
  run: doxygen Doxyfile
- name: Publish to gh-pages
  uses: peaceiris/actions-gh-pages@v3
  with:
    publish_dir: build/doc/html
```

luban itself uses this exact pattern — see `.github/workflows/docs.yml` in
the luban repo for the full mdBook + Doxygen dual-publish.
