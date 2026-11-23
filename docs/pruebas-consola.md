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
- [ ] **1.5. Archivos de los otros que no se pueden leer o son basura.** Prueba cada caso por
  separado, borrando `config.toml` antes de arrancar MaxPack:
  - un `/data/shadowmount/config.ini` con bytes al azar (por ejemplo, una imagen renombrada);
  - un `/data/krakenmount/config.toml` de más de 256 KiB;
  - una carpeta llamada `/data/shadowmount/manual.lst`.
  - Esperado:
    - MaxPack arranca en unos segundos, sin colgarse;
    - el archivo grande da `config: first start: <ruta>: N bytes, over 262144; skipped`, y la
      carpeta, `not a regular file; skipped`;
    - la basura no aporta raíces;
    - los juegos de las raíces de siempre salen igual.
- [ ] **1.6. Una raíz dentro de los datos de otro montador.**
  - Añade a mano `"/data/krakenmount/caches"` a la `scanpath` de `/data/maxpack/config.toml`.
  - Esperado:
    - una sola vez en el registro, `scan root /data/krakenmount/caches is inside another mounter's
      data folder: not scanned (MaxPack never writes there)`;
    - ningún juego de esa carpeta sale en la biblioteca.
  - Quita la línea al acabar.
- [ ] **1.7. Nada escrito en los otros.** Al acabar esta lista, compara por FTP `/data/krakenmount`
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
  - Esperado, entre 2 y 4 s después (MaxPack lo tiene que ver cerrado dos veces, con 2 s entre una
    y otra):
    - aviso «ShadowMount+ se ha cerrado; MaxPack vuelve a montar juegos»;
    - `waiting` vale `null`;
    - los juegos se montan en la siguiente pasada.
- [ ] **2.3. ShadowMountPlus arrancado después.**
  - Con MaxPack montando, arranca ShadowMountPlus.
  - Esperado:
    - en unos 2 s, el aviso y `waiting` como en 2.1;
    - si construía, `pass stopped before <TID>` en el registro y nada más se construye.
  - Lanza un juego de MaxPack. Esperado: `[launch] <TID> other:shadowmountplus.elf: not overlaying
    this launch` y ningún `overlay MOUNTED`.
- [ ] **2.4. Una construcción larga cuando arranca el otro.**
  - Pon un FPKG nuevo (aún sin caché de MaxPack), espera a que el juego salga como «Creando» y
    arranca ShadowMountPlus antes de que acabe.
  - Esperado:
    - al acabar la construcción, `<TID>: built; not registered while another mounter runs` en el
      registro;
    - el juego queda en `pending` en `games`, sin registrar ni montar;
    - al cerrar ShadowMountPlus, la siguiente pasada lo registra y lo monta, sin construirlo otra vez.
- [ ] **2.5. ShadowMountPlus en marcha durante 10 minutos.**
  - Con ShadowMountPlus en marcha, deja MaxPack 10 minutos sin tocar nada.
  - Esperado:
    - `waiting` sigue en `"other:shadowmountplus.elf"` todo el rato;
    - ningún aviso de «vuelve a montar»;
    - en el registro de esos 10 minutos, ninguna línea `nullfs`, `registered`, `overlay MOUNTED` ni
      `is gone; mounting again`.
  - Si sale `process list unreadable`, apúntalo: el estado se queda como estaba y no debe montar.
- [ ] **2.6. Un juego de MaxPack en marcha cuando arranca el otro.**
  - Lanza un juego con MaxPack (en el registro, `overlay MOUNTED`), arranca ShadowMountPlus y sal del
    juego.
  - Esperado: `overlay UNMOUNTED` y `cleanup done` como siempre, y la consola no se cuelga.
  - Repítelo diez veces.
- [ ] **2.7. El otro se cierra con un juego suyo en marcha.**
  - Lanza un juego con ShadowMountPlus y, con el juego abierto, cierra ShadowMountPlus.
  - Esperado:
    - si MaxPack ya tenía caché de ese juego: `not overlaying this launch` al lanzarlo, y MaxPack no
      toca su montaje hasta que salgas del juego (`paused` en `/api/state`);
    - si no la tenía: el título queda en error con `unmount /system_ex/app/<TID>, a nullfs of <ruta>
      (title running?)`, sin forzar nada y sin repetirlo en cada pasada (ver 3.4);
    - salir del juego no cuelga la consola.
- [ ] **2.8. Kraken Mount arrancado después, de las dos formas.**
  - Arranca Kraken Mount original desde su icono del menú. Ciérralo, y arráncalo otra vez enviando su
    ELF al cargador con `nc <ip-de-la-consola> 9021 < kraken-mount.elf`.
  - Esperado en los dos casos:
    - aviso «MaxPack: Kraken Mount está en marcha; MaxPack no monta juegos hasta que se cierre»;
    - `waiting` vale `"other:kraken-mount.elf"`;
    - el aviso de la página nombra `kraken-mount.elf`.
  - Si en alguno no lo detecta, apunta cómo lo arrancaste y, si puedes verlo, el nombre de su proceso.
  - Ciérralo desde su página (Detener). Esperado: aviso «Kraken Mount se ha cerrado; MaxPack vuelve
    a montar juegos» y MaxPack monta otra vez.
- [ ] **2.9. Con el otro en marcha, la página de MaxPack responde.**
  - La biblioteca, los ajustes y el registro se ven.
  - Borrar un juego o expulsar un USB desde MaxPack responde «another mounter is running (…):
    MaxPack touches no game mount until it stops», y no se desmonta ni se borra nada.
  - «Recrear caché», «Recrear todo» y «Purgar caché» quedan en cola hasta que el otro se cierra.
- [ ] **2.10. kstuff deja de responder a mitad de sesión** (solo si puedes provocarlo, por ejemplo
  si tu kstuff se cae tras el modo reposo).
  - Esperado:
    - aviso de kstuff y `waiting` vale `"kstuff"`;
    - nada nuevo se monta ni se construye;
    - cuando kstuff vuelve, en el registro `kstuff: ready; mounting`, y MaxPack sigue.

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
    - un **FPKG**: su nullfs de `/data/krakenmount/caches/<TID>` da `… is a nullfs of
      /data/krakenmount/caches/<TID> (not ours, or stale); unmounting it`, y se monta el de
      `/data/maxpack/caches/<TID>`; se vuelve a extraer la primera vez;
    - un juego **en carpeta**: los dos montan la misma carpeta, así que su montaje **se conserva**
      (sin «unmounting it»); MaxPack solo crea su `fakelib`;
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
  - Pon `Juego [PPSA01234].exfat` y `otro.ffpkg` en `/data/homebrew` y en
    `/mnt/usb0/homebrew/<subcarpeta>/`. Una subcarpeta directa de la raíz del USB no vale: MaxPack
    no baja de la raíz de una unidad.
  - Esperado:
    - MaxPack no las monta;
    - `/api/state` las lista en `images`, `{"path", "kind": "exfat", "title_id": "PPSA01234"}` y
      `{"path", "kind": "ffpkg"}` sin `title_id`;
    - un juego que ShadowMountPlus montó desde una imagen sigue como estaba.
- [ ] **3.6. Un juego que pasa de carpeta a imagen no se quita.**
  - Con un juego en carpeta ya montado por MaxPack, sustituye la carpeta por
    `Juego [<su TITLE_ID>].exfat` en la misma raíz.
  - Esperado: pasado `auto_remove_delay_seconds` (300 s por defecto), el juego **sigue** en el menú.
    No sale `removed from the home screen` para ese título.

## 4. Volver a Kraken Mount y a ShadowMountPlus

- [ ] **4.1. Volver a Kraken Mount.** Detén MaxPack desde su página («Detener MaxPack») y arranca
  Kraken Mount original. Esperado: sus juegos salen y arrancan como antes, con su caché y su
  configuración intactas.
- [ ] **4.2. Volver a ShadowMountPlus.** Lo mismo con ShadowMountPlus.
- [ ] **4.3. Y otra vez a MaxPack.** Esperado: adopta de nuevo sin duplicados, como en 3.1.
