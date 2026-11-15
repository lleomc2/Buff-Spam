# Spam Buff (Geode · GD 2.2081 · Android ARM64)

Mod de Geometry Dash que optimiza el spam. Compatible con Geode 5.11.2.

## Cómo sacar el .geode (sin PC)

1. En github.com (modo "sitio de escritorio" si estás en el celu) creá un repositorio nuevo (puede ser privado).
2. **Add file → Upload files** y subí: `mod.json`, `CMakeLists.txt`, `main.cpp`, `SpamEngine.hpp`, `about.md`, `changelog.md`, `logo.png`. Commit.
3. **Add file → Create new file**, nombre `.github/workflows/build.yml` (con las barras, crea las carpetas) y pegá el contenido de `build.yml`. Commit.
4. Pestaña **Actions** → se ejecuta sola "Compilar Spam Buff". Tarda unos 5–10 min.
5. Entrá a la ejecución en verde → **Artifacts** → bajá `SpamBuff-geode`. Adentro está `lleomc2.spam_buff.geode`.
6. Instalalo igual que tus otros mods de Geode.

Si falla la compilación, copiá el error del paso "Compilar el mod" y mandámelo.

## Con PC

Instalá Geode CLI + Android NDK y corré `geode build -p android64` en esta carpeta.

## Cómo funciona

Hook en `GJBaseGameLayer::handleButton` (entrada) y `processCommands` (un tick por paso de física).
La lógica está en `SpamEngine.hpp` (C++ puro, probada en PC con un simulador).

El id del mod (`lleomc2.spam_buff`) y el developer se cambian en `mod.json`.
