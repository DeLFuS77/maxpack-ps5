<p align="center"><img src="ps5/icon0.png" width="128" alt="MaxPack"></p>

# MaxPack para la consola

MaxPack es la herramienta de juegos de PS5 de DeLFuS77, en su versión para la consola: un payload
(ELF) en Rust que busca juegos en el SSD interno, en la unidad M.2 y en unidades USB, los registra en
la pantalla de inicio y los monta al lanzarlos. Está basado en
[Kraken Mount](https://github.com/quer3q/kraken-mount), de quer3q, y conserva su historial y su licencia.

**Formatos admitidos**: FPKG de depuración (`.pkg`) y carpetas de juego (sin comprimir, o empaquetadas
con zstd o LZ4).

## Estado

**Sin probar en consola.** Esta versión solo cambia la identidad respecto a Kraken Mount (nombre,
rutas, icono, puerto y SDK); el comportamiento es el de Kraken Mount 0.3.1. El payload se compila en
GitHub Actions, pero todavía no se ha ejecutado en una PS5.

## Identidad

- Binario: `maxpack.elf`; copia propia en `/data/maxpack/maxpack.elf`.
- Datos en `/data/maxpack/`: `config.toml`, `maxpack.log`, `caches/<TID>`, `ampr/`.
- Icono de la pantalla de inicio: `MXPK00001`. Interfaz web en el puerto **8095**.
- La parte que habla con [ampr-emu-ng](https://github.com/DeLFuS77/ampr-emu-ng) no cambia de nombre
  (`kraken.image`, `/app0/kraken/`, `kraken.pkg`, `kraken_zero.dat`), y el crate interno sigue
  llamándose `kraken-mount-pkg` para facilitar las mezclas con el original.

## Cómo compilar

```
ps5/build.sh
```

Necesita Docker. Compila en dos etapas (la primera es la copia que arranca el icono, la segunda la
incluye) y deja `target/ps5/maxpack.elf`. La imagen de Docker es `maxpack-ps5` y usa el SDK de PS5
v0.45. La comprobación rápida en el PC, sin Docker:
`cargo check --workspace --locked --target x86_64-unknown-freebsd`. En CI, el trabajo `check` también
ejecuta `tools/check_identity.sh`, que falla si reaparece el nombre o una ruta antiguos.

Para enviarlo a una PS5 con el cargador de ELF (elfldr, puerto 9021):
`nc -w 3 <ip-de-la-ps5> 9021 < maxpack.elf`. kstuff debe estar cargado y ShadowMount+ no debe estar en
ejecución.

## Créditos

- [quer3q/kraken-mount](https://github.com/quer3q/kraken-mount): Kraken Mount, la base de este
  proyecto, y [ampr-emu-ng](https://github.com/quer3q/ampr-emu-ng), del mismo autor.
- [drakmor/ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus): el registro en la pantalla de
  inicio, la superposición de `fakelib` y los backports están portados de ahí.
- [drakmor/dlc_emu](https://github.com/drakmor/dlc_emu): el emulador de DLC incluido.
- [drakmor/pgo_stub](https://github.com/drakmor/pgo_stub): el stub de PlayGo incluido.
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk): la cadena de herramientas de PS5.

## Licencia

GPL-3.0-or-later (`LICENSE`). Los avisos de terceros están en `THIRD_PARTY_NOTICES`.
