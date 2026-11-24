<p align="center"><img src="ps5/icon0.png" width="128" alt="MaxPack"></p>

# MaxPack para la consola

MaxPack es la herramienta de juegos de PS5 de DeLFuS77, en su versión para la consola: un payload
(ELF) en Rust que busca juegos en el SSD interno, en la unidad M.2 y en unidades USB, los registra en
la pantalla de inicio y los monta al lanzarlos, y que además los instala. Está basado en
[Kraken Mount](https://github.com/quer3q/kraken-mount), de quer3q, y conserva su historial y su licencia.

**Formatos admitidos**: FPKG de depuración (`.pkg`), carpetas de juego (sin comprimir, o empaquetadas
con zstd o LZ4) y `.p5z` de MaxPack, que se juegan tal cual, sin instalarlos; el instalador también instala
`.p5z` e imágenes de ShadowMountPlus.

## Estado

**Sin probar en consola.** Nada de esto se ha ejecutado todavía en una PS5: el payload se compila en
GitHub Actions y la lógica que no necesita la consola se prueba en el PC. Lo que hay que comprobar en
la consola está en [`docs/pruebas-consola.md`](docs/pruebas-consola.md). Lo que ya se ha probado,
juego a juego, está en la [lista de compatibilidad](docs/compatibilidad.md) (`docs/compatibilidad.md`).

Lo que hace Kraken Mount 0.3.1, su base, lo hace igual: buscar juegos en las raíces de siempre, la
caché de cada juego, el registro en la pantalla de inicio y el montaje al lanzar, ampr-emu-ng, los
emuladores de DLC y PlayGo, los backports, los juegos movidos, quitar los que desaparecen, expulsar
un USB, los avisos y los ajustes. De lo que Kraken Mount añadió después trae: los juegos que lanzan
un `*.elf` al salir, la página que vuelve sola tras el modo reposo, el icono en FW 12+, el botón
Jugar, los cierres inesperados con su informe, el buscador, el filtro por unidad y «Recrear caché»
apagado hasta que termina. De su 0.3.2: las fuentes de la consola dentro del juego, las «Rutas
excluidas» (en Ajustes: el escaneo nunca las abre) y, en la ficha de un juego en carpeta sin
empaquetar, «Librerías de MaxPack al lanzar», que se puede apagar para un juego que deba ir tal cual
(un `.pkg`, un `.p5z` o una carpeta zstd o LZ4 no pueden ir sin ellas).

Lo que añade MaxPack:
- **El instalador** (pestaña Instalar), el de la fase 2 de MaxPack con su motor en C dentro del
  payload: instala `.p5z`, carpetas e imágenes, con espacio, reanudar, verificar, reemplazar con
  confirmación y desinstalar. No desinstala ni reemplaza con un juego abierto, y al desinstalar un
  juego que monta MaxPack lo quita también de la pantalla de inicio.
- **El modo «solo instalador»**: mientras corren ShadowMountPlus o Kraken Mount, MaxPack no monta
  nada, pero la página y el instalador siguen (lo instalado se registra en ShadowMountPlus, como en la
  fase 2). Al cerrarse el otro, MaxPack vuelve a montar.
- **La adopción**: los juegos que registraron ShadowMountPlus o Kraken Mount se adoptan sin
  duplicarlos, y la primera vez se importan las rutas que se añadieron a mano en ellos. MaxPack no
  escribe nada en `/data/krakenmount` ni en `/data/shadowmount`, así que se puede volver a ellos.
- **Jugar desde el `.p5z`**: un `.p5z` en una raíz de escaneo (o una carpeta más abajo) sale como un
  juego más, sin instalarlo. En el SSD interno solo se extraen sus archivos sueltos
  (`/data/maxpack/caches/<TID>`); los volúmenes se leen del propio `.p5z`, que se monta en solo lectura
  en `/app0/p5z` al lanzar, así que el archivo tiene que seguir donde está. Hace falta espacio para los
  sueltos más 256 MiB: si no cabe, el aviso dice cuánto falta y el juego se monta solo al liberarlo. Un
  `.p5z` dañado o con más de 256 volúmenes no es un juego: sale en «Archivos .p5z que no se pueden
  jugar». Para eso, el motor tiene que decir «dañado» dos pasadas seguidas sobre el mismo archivo: un
  fallo de lectura de un momento (un USB que se reinicia) solo se reintenta, también al construir su
  caché. Si el mismo juego está también en carpeta o en `.pkg`, gana esa copia y el `.p5z` sale como
  duplicado. El nombre del archivo
  puede llevar tildes, ™, ’ o japonés, como los que pone MaxPack en el PC; solo no valen los caracteres
  de control. Siempre va con nuestra ampr-emu-ng, la única que conoce este modo.
- **La caché y el reloj**: cada caché apunta el tamaño y la fecha de los archivos de los que sale
  (el `.pkg`, el `.p5z`, el backport, los de la carpeta) y se rehace solo si alguno cambia. No los
  compara con el reloj de la consola, que en una consola sin conexión suele ir años atrasado.
- **Nuestra ampr-emu-ng**: la incluida es una compilación propia (`v0.5.1-maxpack.1`: la v0.5.1 de quer3q con el modo `.p5z`), de
  [DeLFuS77/ampr-emu-ng](https://github.com/DeLFuS77/ampr-emu-ng) (`vendor/ampr-emu-ng/SOURCE`). La
  actualización automática está apagada hasta que se publique.
- **La biblioteca de la página**: «Jugar» arranca un juego listo desde la ficha (también desde el
  móvil), «Copiar informe» da un texto para una incidencia con el firmware, el último cierre
  inesperado y la fila para la [lista de compatibilidad](docs/compatibilidad.md), y un juego se puede
  ocultar de la biblioteca (sigue montado y en la pantalla de inicio; «Mostrar ocultos» lo enseña).
  Antes de instalar, cada unidad dice si el juego cabe, y si no cabe no empieza la copia.
- **Protecciones**: nunca registra ni monta un título que esté instalado en la consola, nunca explora
  carpetas del sistema ni de datos, y el puerto 8095 solo atiende a la página abierta con la IP de la
  consola (o `localhost`), no a otras webs.

> **El `maxpack.elf` de la fase 2.** Se llama igual y usa el mismo puerto, pero no sabe dejarle el
> sitio a una copia nueva. Si sigue en alguna lista de carga automática, arranca antes que este, y
> este se cierra con el aviso «puerto 8095 ocupado». Quítalo de esas listas y reinicia la consola
> antes de probar este.

## Identidad

- Binario: `maxpack.elf`; copia propia en `/data/maxpack/maxpack.elf`.
- Datos en `/data/maxpack/`: `config.toml`, `maxpack.log`, `caches/<TID>`, `ampr/`, `crashes.json`, y los del
  instalador (`state/`, `installed/`, `tmp/`).
- Icono de la pantalla de inicio: `MXPK00001`. Interfaz web en el puerto **8095**.
- Versión propia, la de `Cargo.toml` (de ahí la toman la página y `/api/state`): la 1.0.0 es la primera
  publicada, con las dos etapas (montar y jugar desde el `.p5z`); las 0.1.x y 0.2.x fueron las de prueba.
  No sigue la numeración de Kraken Mount, cuya 0.3.1 es la base.
- La parte que habla con [ampr-emu-ng](https://github.com/DeLFuS77/ampr-emu-ng) no cambia de nombre
  (`kraken.image`, `/app0/kraken/`, `kraken.pkg`, `kraken_zero.dat`), y el crate interno sigue
  llamándose `kraken-mount-pkg` para facilitar las mezclas con el original.

## Cómo compilar

```
ps5/build.sh
```

Necesita Docker. Compila en dos etapas (la primera es la copia que arranca el icono, la segunda la
incluye) y deja `target/ps5/maxpack.elf`. La imagen de Docker es `maxpack-ps5` y usa el SDK de PS5
v0.45. Las comprobaciones rápidas en el PC, sin Docker:
`cargo check --workspace --locked --target x86_64-unknown-freebsd` y
`cargo test -p maxpack-logic`. En CI, el trabajo `check` también ejecuta `tools/check_identity.sh`,
que falla si reaparece el nombre o una ruta antiguos.

Para enviarlo a una PS5 con el cargador de ELF (elfldr, puerto 9021):
`nc -w 3 <ip-de-la-ps5> 9021 < maxpack.elf`. kstuff debe estar cargado. Si ShadowMount+ o Kraken
Mount están en marcha, MaxPack solo instala hasta que se cierren.

## Créditos

- [quer3q/kraken-mount](https://github.com/quer3q/kraken-mount): Kraken Mount, la base de este
  proyecto, y [ampr-emu-ng](https://github.com/quer3q/ampr-emu-ng), del mismo autor, de la que
  MaxPack incluye una compilación propia.
- [drakmor/ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus): el registro en la pantalla de
  inicio, la superposición de `fakelib` y los backports están portados de ahí.
- [drakmor/dlc_emu](https://github.com/drakmor/dlc_emu): el emulador de DLC incluido.
- [drakmor/pgo_stub](https://github.com/drakmor/pgo_stub): el stub de PlayGo incluido.
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk): la cadena de herramientas de PS5.
- [zstd](https://github.com/facebook/zstd) y [zlib](https://zlib.net), en el motor del instalador, y
  [SQLite](https://www.sqlite.org).

## Licencia

GPL-3.0-or-later (`LICENSE`), sin garantía. Los avisos de terceros están en `THIRD_PARTY_NOTICES`.
