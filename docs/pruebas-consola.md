# Pruebas en la consola

Lista para probar MaxPack en una PS5, como la P1–P5 de la fase 2. Va por partes; cada una dice qué
hacer y qué debe pasar. Marca cada casilla al comprobarla y apunta el firmware.

Dónde mirar:
- el registro, `/data/maxpack/maxpack.log` (también en la página, al final);
- el estado, `http://<ip-de-la-consola>:8095/api/state`: los campos `waiting`, `games` e `images`;
- los avisos de la consola.

> Trabaja sobre copias. Guarda antes `/data/krakenmount` y `/data/shadowmount` por FTP: MaxPack no
> debe tocarlos, y así puedes compararlos.

## 0. Antes de nada

- [ ] **0.1. Kraken Mount original funciona en tu firmware con un juego de MaxPack.**
  - Copia a `/data/homebrew/<TITLE_ID>` (o a `homebrew/` de un USB) un juego empaquetado con
    `maxpack pack` (carpeta con `ampr_zstd.index`).
  - Arranca solo Kraken Mount original (de quer3q, 0.3.1), sin ShadowMountPlus ni MaxPack.
  - Esperado: el juego sale en el menú, arranca, sale «ampr-emu-ng: v… (zstd) running», carga y
    guarda partida.
  - Si esto falla, lo demás no sirve: para, y apunta el firmware y el registro de Kraken Mount.
- [ ] **0.2. Cierra Kraken Mount** desde su página (Detener) antes de seguir.

## 1. Raíces importadas (la primera vez)

- [ ] **1.1. Con rutas propias en los otros montadores.**
  - Prepara:
    - una `scanpath` propia en `/data/krakenmount/config.toml`, por ejemplo `/mnt/usb0/juegos`;
    - una línea `path=` en la sección `[scan]` de `/data/shadowmount/config.ini`;
    - la carpeta de un juego en `/data/shadowmount/manual.lst`, fuera de las raíces de siempre.
  - Borra `/data/maxpack/config.toml` (o usa una consola donde MaxPack no haya corrido) y arranca
    MaxPack.
  - Esperado:
    - en el registro, `config: first start: imported N scan root(s) from the other mounters: [...]`
      con esas rutas (de `manual.lst`, la carpeta **padre** del juego);
    - `config.toml` tiene `scanpath` con las raíces de siempre primero (de todas las unidades,
      estén o no) y después las importadas;
    - salen los juegos de esas rutas y también los de `/data/homebrew` y del USB.
- [ ] **1.2. Sin rutas propias.**
  - Borra `config.toml` y arranca MaxPack en una consola sin rutas propias en los otros montadores.
  - Esperado:
    - `imported 0 scan roots` en el registro;
    - `config.toml` sin línea `scanpath` (solo el ejemplo comentado);
    - un USB enchufado después se explora.
- [ ] **1.3. Solo la primera vez.** Cambia la `scanpath` del otro montador y reinicia MaxPack.
  Esperado: `config.toml` no cambia.
- [ ] **1.4. USB enchufado después, con raíces importadas.** Con el `config.toml` de 1.1, enchufa un
  USB que no estaba al arrancar. Esperado: sus `homebrew/` y `etaHEN/games` se exploran.
- [ ] **1.5. Nada escrito en los otros.** Al acabar esta lista, compara por FTP `/data/krakenmount`
  y `/data/shadowmount` con la copia del principio. Esperado: los mismos archivos, tamaños y fechas.

## 2. Otro montador

- [ ] **2.1. ShadowMountPlus arrancado antes.** Arranca ShadowMountPlus y luego MaxPack.
  - Esperado:
    - aviso «ShadowMount+ está en ejecución»;
    - la página de MaxPack abre y lo dice;
    - `waiting` vale `"other:shadowmountplus.elf"`;
    - en el registro, `shadowmountplus.elf is running: no scan, build, register or new overlay`;
    - MaxPack no registra ni monta nada mientras tanto.
- [ ] **2.2. ShadowMountPlus se cierra.**
  - Esperado, en unos 2 s:
    - aviso «ShadowMount+ se ha cerrado; MaxPack vuelve a montar juegos»;
    - `waiting` vale `null`;
    - los juegos se montan en la siguiente pasada.
- [ ] **2.3. ShadowMountPlus arrancado después.**
  - Con MaxPack montando (mejor durante una primera construcción larga), arranca ShadowMountPlus.
  - Esperado:
    - en unos 2 s, el aviso y `waiting` como en 2.1;
    - si construía, `pass stopped before <TID>` en el registro y nada más se construye.
  - Lanza un juego de MaxPack. Esperado: `[launch] <TID> other:shadowmountplus.elf: not overlaying
    this launch` y ningún `overlay MOUNTED`.
- [ ] **2.4. Un juego en marcha cuando arranca el otro.**
  - Lanza un juego con MaxPack (en el registro, `overlay MOUNTED`), arranca ShadowMountPlus y sal del
    juego.
  - Esperado: `overlay UNMOUNTED` y `cleanup done` como siempre, y la consola no se cuelga.
  - Repítelo diez veces.
- [ ] **2.5. Kraken Mount arrancado después.** Lo mismo que 2.3 con Kraken Mount original.
  - Esperado al arrancarlo:
    - aviso «MaxPack: Kraken Mount está en marcha; MaxPack no monta juegos hasta que se cierre»;
    - `waiting` vale `"other:kraken-mount.elf"`;
    - el aviso de la página nombra `kraken-mount.elf`.
  - Ciérralo desde su página (Detener). Esperado: aviso «Kraken Mount se ha cerrado; MaxPack vuelve
    a montar juegos» y MaxPack monta otra vez.
- [ ] **2.6. Con el otro en marcha, la página de MaxPack responde.** La biblioteca, los ajustes y el
  registro.

## 3. Adoptar juegos de ShadowMountPlus y de Kraken Mount

- [ ] **3.1. Juegos de ShadowMountPlus, tras reiniciar.**
  - Con varios juegos en carpeta registrados por ShadowMountPlus, reinicia la consola y arranca solo
    MaxPack.
  - Esperado:
    - cada juego sale **una sola vez** en el menú;
    - se crea `/data/maxpack/caches/<TID>` (en una carpeta, solo `fakelib`);
    - el juego arranca con `overlay MOUNTED` en el registro.
- [ ] **3.2. Juegos de ShadowMountPlus, en la misma sesión.**
  - ShadowMountPlus montó sus juegos; ciérralo y arranca MaxPack, sin reiniciar.
  - Esperado:
    - un juego en carpeta de la misma ruta conserva su montaje;
    - uno con otra fuente da `/system_ex/app/<TID> is a nullfs of <su ruta> (not ours, or stale);
      unmounting it` y se monta el de MaxPack;
    - sin duplicados en el menú.
- [ ] **3.3. Juegos de Kraken Mount, en la misma sesión.**
  - Con juegos FPKG y en carpeta registrados por Kraken Mount, ciérralo y arranca MaxPack.
  - Esperado:
    - el nullfs de su caché se desmonta y se monta el de `/data/maxpack/caches/<TID>`;
    - un FPKG se vuelve a extraer la primera vez;
    - sin duplicados en el menú;
    - los juegos arrancan.
- [ ] **3.4. Un montaje que no se puede quitar.**
  - Si un juego de otro montador sigue en marcha cuando MaxPack lo adopta, el título queda en error.
    El motivo dice `unmount /system_ex/app/<TID>, a nullfs of <ruta> (title running?)`.
  - Esperado:
    - nunca se fuerza;
    - el error no se repite en cada pasada;
    - al salir del juego, «Recrear caché» en la página (o reiniciar MaxPack) lo arregla.
- [ ] **3.5. Imágenes de ShadowMountPlus.**
  - Pon `Juego [PPSA01234].exfat` y `otro.ffpkg` en `/data/homebrew` y en una subcarpeta de un USB.
  - Esperado:
    - MaxPack no las monta;
    - `/api/state` las lista en `images`, `{"path", "kind": "exfat", "title_id": "PPSA01234"}` y
      `{"path", "kind": "ffpkg"}` sin `title_id`;
    - un juego que ShadowMountPlus montó desde una imagen sigue como estaba.

## 4. Volver a Kraken Mount y a ShadowMountPlus

- [ ] **4.1. Volver a Kraken Mount.** Detén MaxPack desde su página («Detener MaxPack») y arranca
  Kraken Mount original. Esperado: sus juegos salen y arrancan como antes, con su caché y su
  configuración intactas.
- [ ] **4.2. Volver a ShadowMountPlus.** Lo mismo con ShadowMountPlus.
- [ ] **4.3. Y otra vez a MaxPack.** Esperado: adopta de nuevo sin duplicados, como en 3.1.
