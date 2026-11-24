# Compatibilidad

Qué juegos se han probado con MaxPack, en qué consola y con qué resultado. La lista es pública:
cualquiera puede añadir su prueba. Una fila por juego, versión, firmware y montador; si el resultado
cambia con otra versión de MaxPack o del juego, va en una fila nueva y la anterior se queda.

*English: a per-title compatibility list (title id, game, version, source kind, firmware, mounter,
result, date). The page's "Copy report" gives a ready row.*

## Cómo añadir una fila

1. En la página de MaxPack, abre la ficha del juego y pulsa «Copiar informe». El informe trae al final
   la fila hecha: `TITLE_ID`, nombre, versión, origen, firmware y montador.
2. Rellena el resultado y la fecha. La fecha, la de verdad: el reloj de una consola sin conexión suele
   ir años atrasado, por eso el informe no la pone.
3. Añade la fila a la tabla, por orden de `TITLE_ID`.

Columnas:
- **Title ID**: el de `sce_sys/param.json` (`PPSA…`, `CUSA…`).
- **Juego**: el nombre que sale en la pantalla de inicio.
- **Versión**: `contentVersion` de `param.json`.
- **Origen**: `pkg` (FPKG de depuración), `carpeta` (sin comprimir), `carpeta zstd` o `carpeta lz4`
  (empaquetada) o `p5z` (archivo de MaxPack, jugado sin instalar). Entre paréntesis, con qué se
  empaquetó cuando importa.
- **Firmware**: el de la consola (Ajustes > Sistema > Software del sistema).
- **Montador**: el que montó el juego, con su versión (`MaxPack 0.2.0`, `ShadowMountPlus 1.7beta12`…).
  MaxPack etapa 1 es la 0.1.x (montar) y etapa 2 la 0.2.x (jugar desde el `.p5z`).
- **Resultado**: lo que se comprobó (arranca, guarda, carga, se juega hasta tal punto) y lo que falla.
- **Fecha**: la de la prueba, `AAAA-MM-DD`.

## Lista

| Title ID | Juego | Versión | Origen | Firmware | Montador | Resultado | Fecha |
|---|---|---|---|---|---|---|---|
| PPSA12544 | Hollow Knight: Silksong | 01.000.010 | carpeta zstd (empaquetada con MaxPack) | 13.60 | ShadowMountPlus 1.7beta12 y MaxPack etapa 1 | Arranca, guarda y carga partida | 2026-11-24 |
