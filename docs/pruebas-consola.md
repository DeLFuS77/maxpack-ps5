# Pruebas en la consola

Lista para probar MaxPack en una PS5, como la P1–P5 de la fase 2. Va por partes; cada una dice qué
hacer y qué debe pasar. Marca cada casilla al comprobarla y apunta el firmware.

Dónde mirar:
- el registro, `/data/maxpack/maxpack.log` (también en la página, al final);
- el estado, `http://<ip-de-la-consola>:8095/api/state`: los campos `waiting`, `games` (con
  `kind == "p5z"` para un `.p5z`, y `error_code` y `missing_bytes` si no cabe), `images` y `unplayable`
  (los `.p5z` que no se pueden jugar);
- el del instalador, `http://<ip-de-la-consola>:8095/api/inst/state`: `job`, `pending`, `installed`
  y `last`;
- los avisos de la consola.

> Trabaja sobre copias. Guarda antes `/data/krakenmount` y `/data/shadowmount` por FTP: MaxPack no
> debe tocarlos, y así puedes compararlos.

## 0. Antes de nada

- [ ] **0.0. Sin el `maxpack.elf` de la fase 2.**
  - Quita el `maxpack.elf` de la fase 2 de toda lista de carga automática (la del cargador, la de
    etaHEN o la de kstuff) y reinicia la consola. Se llama igual y usa el mismo puerto 8095, pero no
    sabe dejarle el sitio a una copia nueva.
  - Esperado: al enviar este MaxPack no sale el aviso «MaxPack: puerto 8095 ocupado, no se ha
    iniciado…», y la página abre en `http://<ip-de-la-consola>:8095`.
  - Si sale ese aviso, el de la fase 2 (u otra copia) sigue en marcha: reinicia sin él y vuelve a
    empezar. No sigas con él en marcha: lo que veas en el puerto 8095 sería suyo.
- [ ] **0.1. Kraken Mount original funciona en tu firmware con un juego de MaxPack.**
  - Copia a `/data/homebrew/<TITLE_ID>` (o a `homebrew/` de un USB) un juego empaquetado con
    `maxpack pack` (carpeta con `ampr_zstd.index`).
  - Arranca solo Kraken Mount original (de quer3q, 0.3.1), sin ShadowMountPlus ni MaxPack.
  - Esperado: el juego sale en el menú, arranca, sale «ampr-emu-ng: v… (zstd) running», carga y
    guarda partida.
  - Si esto falla, lo demás no sirve: para, y apunta el firmware y el registro de Kraken Mount.
- [ ] **0.2. Cierra Kraken Mount** desde su página (Detener) antes de seguir.

## B. Base (modo normal)

Lo que hace Kraken Mount, con MaxPack solo (sin ShadowMountPlus ni Kraken Mount en marcha). Prepara
un juego de cada clase de fuente: una carpeta sin comprimir, una empaquetada con zstd, una con LZ4 y
un FPKG (`.pkg`).

- [ ] **B.1. Lanzar y salir diez veces de cada clase de fuente.**
  - Con cada uno de los cuatro juegos: lánzalo, espera a que cargue, sal al menú, y así diez veces.
  - Esperado, cada vez: en el registro, `<TID> overlay MOUNTED` al lanzar y `<TID> cleanup done` al
    salir; la consola no se cuelga ni se apaga, y el juego vuelve a arrancar.
  - Apunta qué juego y qué vez si algo falla.
- [ ] **B.2. Al salir, nada forzado.** En el registro de B.1, busca `(MNT_FORCE)` o `cleanup done
  after MNT_FORCE`.
  - Esperado: no sale ninguno.
  - Si sale: una capa siguió ocupada y MaxPack la quitó a la fuerza. Apunta el juego, la clase de
    fuente, el firmware y las líneas del registro de ese `<TID>` desde `overlay MOUNTED` hasta
    `cleanup done`; repite ese juego cinco veces más. Si la consola se cuelga al salir, para aquí y
    guarda `maxpack.log`.
- [ ] **B.3. Aviso de ampr-emu-ng.**
  - Lanza el juego en zstd.
  - Esperado: el aviso dice «ampr-emu-ng: v0.5.0 (zstd) running», **sin** `-beta`; con el de LZ4,
    «(lz4)». En Ajustes, ampr-emu-ng sale como incluida, versión `v0.5.0-maxpack.3`.
  - Esta compilación sale sin `-beta` (la de `main`, la de la etapa 1, sí lo llevaba: `v0.5.0-beta.a52648f`).
    Si sale `-beta` u otra versión, no es la de esta rama: mira `/data/maxpack/ampr/libSceAmpr.sprx` y
    `/data/maxpack/ampr.bundled`, y apúntalo.
- [ ] **B.4. Emuladores de DLC y PlayGo.**
  - Con «Añadir los emuladores incluidos de DLC (dlc_emu) y PlayGo (pgo_stub)…» activado, lanza un
    juego con DLC en `/user/addcont/<TID>` y sin emulador propio. Esperado: el DLC sale en el juego.
  - Un juego que usa PlayGo: en su ficha sale «Stub de PlayGo», y el juego no pide terminar la
    descarga. Apágalo y lanza otra vez. Esperado: en el registro, `<TID>: PlayGo stub off`, y la
    caché se recrea sin él (`fakelib: PlayGo stub off`).
- [ ] **B.5. Backports.** Pon un backport en `<raíz>/backports/<TID>` de un juego en carpeta.
  Esperado: el juego arranca con el backport aplicado (la versión del backport), y en `/api/state`
  su `backport` es esa carpeta.
- [ ] **B.6. Un juego movido.** Mueve un juego en carpeta de `/data/homebrew` a `homebrew/` de un
  USB. Esperado: aviso «<juego> movido a <ruta>» (sin reconstruir la caché) y el juego arranca desde
  el USB.
- [ ] **B.7. Quitar los que desaparecen, a los 300 s.**
  - Con «Quitar un juego de la pantalla de inicio si falta su origen» activado, saca la carpeta de un
    juego de las raíces que se exploran (por ejemplo, a `/data/aparte`; dentro de una raíz contaría
    como movido) o desenchufa el USB que lo tiene.
  - Esperado: a los 300 s (`auto_remove_delay_seconds`), `<TID>: source <ruta> missing for N s,
    removed from the home screen`, y el juego deja el menú. Antes de eso sigue en el menú.
  - Devuelve la carpeta: el juego vuelve a registrarse en la siguiente pasada.
- [ ] **B.8. Expulsar un USB.** En la biblioteca, «Desmontar» en la píldora del USB. Esperado:
  «USB 1 se desmontó. Ya puedes desconectar la unidad.», sus juegos salen de la biblioteca, y al
  enchufarlo otra vez vuelven.
- [ ] **B.9. Idioma de los avisos.** Cambia el idioma de la consola a inglés y luego a español, y
  provoca un aviso cada vez (por ejemplo, «Recrear caché» de un juego). Esperado: el aviso sale en el
  idioma de la consola.
- [ ] **B.10. Ajustes.**
  - Cambia el intervalo de escaneo y «Guardar». Esperado: «Guardado.», y al volver a abrir Ajustes
    sigue cambiado; `/data/maxpack/config.toml` lo tiene.
  - Pon `/system_ex/app` (o `/`) en «Rutas de escaneo» y «Guardar». Esperado: «Error al guardar:
    MaxPack nunca explora /system_ex/app: …», y `config.toml` no cambia.
  - En Ajustes, «Acerca de» nombra MaxPack, Kraken Mount (quer3q), ampr-emu-ng, ShadowMountPlus,
    dlc_emu, pgo_stub, ps5-payload-sdk, zstd, zlib, GPL-3.0-or-later y «sin garantía».
- [ ] **B.11. Arrancar desde el icono de MaxPack.** Detén MaxPack desde su página («Detener») y abre
  el icono de MaxPack (`MXPK00001`) en la pantalla de inicio.
  - Esperado: la página sale enseguida (guardada), dice «Iniciando MaxPack...» y en unos segundos
    funciona: el cargador arrancó `/data/maxpack/maxpack.elf`.
  - Si dice «maxpack.elf aún no está guardado», envía el ELF una vez al puerto 9021 y repite.
- [ ] **B.12. Un juego volcado cuyo TITLE_ID está instalado en la consola.**
  - Pon en `/data/homebrew/<TITLE_ID>` la carpeta volcada de un juego que también tengas instalado
    (de la tienda o de disco, o un FPKG instalado desde los ajustes de depuración).
  - Esperado:
    - en la biblioteca sale con «Error», y su ficha dice «Este título está instalado en la consola
      (/user/app/<TITLE_ID>/app.pkg): MaxPack no registra ni monta esta copia…»;
    - en el registro, una vez, `<TITLE_ID> (<ruta>): an installed copy of this title exists (…):
      MaxPack does not register or mount this copy`;
    - el juego instalado sigue igual: su icono, y al lanzarlo arranca el instalado, sin
      `overlay MOUNTED` de ese `<TITLE_ID>` en el registro.
  - Quita la carpeta al acabar.
- [ ] **B.13. Otra web no puede tocar MaxPack.** Desde un PC de la misma red:
  - `curl -i -X POST -H "Origin: http://ejemplo.com" http://<ip-de-la-consola>:8095/api/rescan`.
    Esperado: `403` y «petición rechazada: no viene de la página de MaxPack».
  - `curl -i -H "Host: ps5.ejemplo" http://<ip-de-la-consola>:8095/api/state`. Esperado: `403` y
    «abre esta página con la dirección IP de la consola».
  - `curl -i -X POST http://<ip-de-la-consola>:8095/api/rescan` (sin `Origin`). Esperado: `204`.
  - En el registro, `web: refused …` (una línea por minuto como mucho).

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

## 5. El instalador (pestaña Instalar)

Prepara en un USB un `.p5z` hecho con `maxpack pack --p5z` y una imagen `.exfat` de otro juego,
los dos en la raíz del USB. Al arrancar, el registro dice `installer: ready in N ms (MaxPack engine
5780807, runtime ampr-emu-ng v0.5.0-maxpack.3 712624 bytes)`.

- [ ] **5.1. Instalar un `.p5z` y un `.exfat` tal cual.**
  - En Instalar salen los dos en «Juegos para instalar»: el `.p5z` con su icono, lo que ahorra y lo
    que ocupará; el `.exfat` con «se copia tal cual».
  - Instala el `.p5z` en el almacenamiento interno y, cuando acabe, el `.exfat` en el USB.
  - Esperado:
    - el progreso avanza con velocidad y tiempo restante, y al final «<juego> instalado»;
    - los dos salen en «Instalados con MaxPack» y en `/data/maxpack/installed/<TITLE_ID>.json`;
    - el `.p5z` queda en `/data/homebrew/<TITLE_ID>` y MaxPack lo monta en su siguiente pasada:
      sale en el menú y arranca;
    - el `.exfat` queda en `homebrew/` del USB y MaxPack **no** lo monta: en Instalar dice «Imagen:
      por ahora se juega con ShadowMountPlus; MaxPack no la monta» (y su «Pendiente en
      ShadowMountPlus» si no responde), y sale en la biblioteca en «Imágenes de ShadowMountPlus»;
    - «Verificar» acaba en «<juego> verificado» con `0 con fallos`.
- [ ] **5.2. Instalar con ShadowMountPlus en marcha (solo instalador).**
  - Arranca ShadowMountPlus y luego MaxPack.
  - Esperado:
    - el aviso de la página dice «Hay otro montador en marcha (shadowmountplus.elf): MaxPack solo
      instala» y que la página se abre en el navegador en `http://<ip-de-la-PS5>:8095`;
    - el icono de MaxPack (`MXPK00001`) **no** se registra: en el registro, `tile: not registered
      while another mounter runs`; en FW 12+ no se llama a `sceAppInstUtilAppInstallAll`;
    - abre `http://<ip-de-la-PS5>:8095` en el navegador de la consola: Instalar funciona;
    - instala un juego en carpeta con packs zstd: queda registrado en ShadowMountPlus con el aviso
      del modo `fakelib`, como en la fase 2, y ShadowMountPlus lo monta;
    - MaxPack no monta nada mientras tanto (`waiting` sigue en `"other:shadowmountplus.elf"`).
  - Cierra ShadowMountPlus. Esperado: el icono se registra entonces (`tile MXPK00001: ...`) y
    MaxPack monta lo instalado.
- [ ] **5.3. Reemplazar.**
  - Copia a mano una carpeta del mismo juego a `/data/homebrew/<TITLE_ID>` (u otra ruta que exploren
    los cargadores) e instala su `.p5z`.
  - Esperado: «¿Instalar y reemplazar?» con la ruta y el tamaño de la copia vieja. «Cancelar» no
    toca nada; «Instalar y reemplazar» instala, verifica y solo entonces borra la vieja. Queda una
    sola copia y una sola entrada en el menú.
- [ ] **5.4. Reanudar tras quitar el USB.**
  - Empieza a instalar un juego grande desde el USB y desconecta el USB a mitad.
  - Esperado: la instalación falla sin colgar MaxPack y sale en «Instalaciones a medias» con lo ya
    copiado.
  - Vuelve a conectar el USB y pulsa «Seguir». Esperado: sigue desde donde se quedó (no empieza de
    cero) y termina como en 5.1.
  - Repite con «Descartar»: se borra lo copiado (`/data/maxpack/tmp/<TITLE_ID>` o `maxpack-tmp/` del
    USB) y desaparece de la lista.
- [ ] **5.5. Salir con una instalación en marcha.**
  - Con una instalación copiando, pulsa «Detener» en la página (o envía otra vez el ELF al cargador:
    la copia nueva pide salir a la vieja).
  - Esperado:
    - en el registro, `installer: pausing before quitting` y `installer: paused; ...` en menos de
      3 s; la copia nueva toma el puerto 8095 sin `port_busy`;
    - al arrancar otra vez, la instalación sale en «Instalaciones a medias» y «Seguir» la termina.
- [ ] **5.6. La página desde el móvil.**
  - Abre `http://<ip-de-la-PS5>:8095` en el navegador de un móvil de la misma red.
  - Esperado: la biblioteca, Instalar y los ajustes se ven sin desplazarse de lado; instalar,
    pausar, verificar y desinstalar funcionan igual que en la consola.
  - Ábrela con un nombre en lugar de la IP (por ejemplo, el de tu router para la consola).
    Esperado: la página entera se rechaza con «MaxPack: abre esta página con la dirección IP de la
    consola…»: solo vale la IP (o `localhost`). Con la IP, y otro montador en marcha, el aviso dice
    `http://<ip-de-la-PS5>:8095`.
- [ ] **5.7. Otro montador arranca durante una instalación.**
  - Con MaxPack montando, empieza a instalar un `.p5z` grande y, a mitad de la copia, arranca
    ShadowMountPlus.
  - Esperado:
    - en unos 2 s, el aviso de solo instalador y `waiting` = `"other:shadowmountplus.elf"`;
    - la copia **sigue** (el porcentaje no se para; nadie la pausa);
    - al final, «Registrando <juego> en ShadowMountPlus» y el juego queda registrado en
      ShadowMountPlus, con el aviso del modo `fakelib`; ShadowMountPlus lo monta;
    - MaxPack no monta nada de ese juego mientras tanto: en el registro, ninguna línea `nullfs`,
      `registered` ni `overlay MOUNTED` con su TITLE_ID.
  - Cierra ShadowMountPlus. Esperado: MaxPack lo adopta como en 3.2, sin duplicarlo.
- [ ] **5.8. Solo instalador con Kraken Mount.**
  - Arranca Kraken Mount original (como en 2.8) y luego MaxPack.
  - Esperado:
    - el aviso dice «Hay otro montador en marcha (kraken-mount.elf): MaxPack solo instala» con
      `http://<ip-de-la-PS5>:8095`, y el icono de MaxPack no se registra (como en 5.2);
    - Instalar funciona; sin ShadowMountPlus no sale «ShadowMountPlus … conectado», y un juego en
      carpeta o un `.p5z` instalado no muestra «Pendiente en ShadowMountPlus» (una imagen sí);
    - lo instalado en `homebrew/` lo recoge Kraken Mount en su siguiente escaneo si esa raíz es suya
      (una imagen no: solo ShadowMountPlus las monta); MaxPack no monta nada.
- [ ] **5.9. Salir con un trabajo en cola y durante una desinstalación.** Apunta lo que pase: así se
  comporta hoy.
  - Instala dos juegos seguidos (el segundo sale como «1 más en cola») y pulsa «Detener» mientras se
    copia el primero.
    - Esperado hoy: el primero se pausa y sale en «Instalaciones a medias» al arrancar otra vez. El
      segundo puede empezar en esos 3 s y quedar también a medias (se sigue con «Seguir»); si no llegó
      a empezar, no queda en ningún sitio y hay que volver a instalarlo.
  - Desinstala un juego grande y pulsa «Detener» enseguida.
    - Esperado hoy: pausar no para una desinstalación; en el registro, `installer: still busy after
      3 s; quitting anyway` y MaxPack sale. Al arrancar otra vez, el juego puede seguir en «Instalados
      con MaxPack» con la carpeta a medio borrar: «Desinstalar» otra vez la termina (carpeta, registro
      y su entrada en ShadowMountPlus).
- [ ] **5.10. Primer arranque sin kstuff, y cuándo sale el icono.**
  - Con la consola recién encendida y sin kstuff cargado, arranca MaxPack.
  - Esperado:
    - aviso de kstuff y, en la página, «Esperando a kstuff»; Instalar responde
      (`installer: ready` en el registro);
    - ninguna línea `tile` todavía: el icono espera a kstuff.
  - Carga kstuff. Esperado: `kstuff: ready; mounting` y después `tile MXPK00001: …` (registrado o
    `up to date`); si en ese momento corre otro montador, `tile: not registered while another
    mounter runs` y el icono llega cuando ese se cierre (5.2).

- [ ] **5.11. Desinstalar en modo normal.**
  - Con MaxPack montando (sin otro montador), desinstala un juego instalado con MaxPack que ya esté
    en el menú (de 5.1).
  - Esperado:
    - al acabar, en el registro, `<TITLE_ID>: uninstalled by the installer (<carpeta>): unmounted
      and removed from the home screen`;
    - el juego deja el menú enseguida, no a los 300 s; sale de la biblioteca;
      `/data/maxpack/caches/<TITLE_ID>` ya no está;
    - lo mismo con «Quitar un juego de la pantalla de inicio si falta su origen» desactivado.
- [ ] **5.12. Desinstalar o reemplazar con el juego abierto.**
  - Abre un juego de MaxPack. Con el juego abierto, en Instalar pulsa «Desinstalar» en un juego
    instalado.
  - Esperado: «Cierra el juego antes.», nada se borra, y en el registro `installer: /api/uninstall
    refused: a game is running`.
  - Igual con «Instalar y reemplazar» (como en 5.3): «Cierra el juego antes.» y la copia vieja sigue.
  - Cierra el juego: las dos cosas funcionan.
- [ ] **5.12b. Un reemplazo largo con la copia vieja abierta.**
  - Empieza «Instalar y reemplazar» de un juego grande y, mientras todavía copia, abre la copia vieja.
  - Anota qué pasa cuando termina la copia y cuando sales del juego. Esperado hoy: el bloqueo solo se
    comprueba al recibir la petición y el motor borra la copia vieja al final, así que los ficheros
    del juego pueden desaparecer bajo él.
  - Anota si la consola se cuelga: eso sería un arreglo obligatorio.
- [ ] **5.12c. Desinstalar con el juego abierto bajo ShadowMountPlus (solo instalador).**
  - Con ShadowMountPlus montando y MaxPack en modo solo instalador, abre un juego instalado desde una
    carpeta del motor y pulsa «Desinstalar» en él.
  - Esperado: se rechaza con «Cierra el juego antes.» (en el registro `installer: /api/uninstall
    refused: a game is running`) y nada se borra. Cierra el juego: ya funciona.
- [ ] **5.13. Desinstalar con otro montador en marcha.**
  - Con ShadowMountPlus en marcha (solo instalador), desinstala un juego instalado con MaxPack.
  - Esperado: el instalador lo quita de ShadowMountPlus como en la fase 2; MaxPack no toca el menú
    (en el registro, `uninstalled by the installer while MaxPack does not mount: its home entry is
    left to the mounter in charge`).

## 6. Jugar desde el .p5z

Un `.p5z` de MaxPack (hecho con `maxpack pack --p5z`) en una raíz de escaneo se juega sin instalarlo:
MaxPack extrae solo sus archivos sueltos a `/data/maxpack/caches/<TITLE_ID>` y lee los volúmenes del
propio `.p5z`, montado en solo lectura en `/app0/p5z` al lanzar. Usa un juego de un tamaño razonable
(unos GB) y, para el espacio, uno cuyos sueltos conozcas.

MaxPack no monta mientras otro montador corre: prueba **primero con ShadowMountPlus cerrado** (6.11 es
la prueba con él en marcha).

- [ ] **6.1. El aviso de ampr-emu-ng.**
  - Copia el `.p5z` a `/data/homebrew` y espera a que salga en el menú; en `/api/state` su `kind` es
    `"p5z"`. Lánzalo.
  - Esperado: el aviso dice «ampr-emu-ng: v0.5.0 (p5z) running» (sin `-beta`); en el registro,
    `<TID>: building cache` al principio y `<TID> overlay MOUNTED` al lanzar; en
    `/data/maxpack/caches/<TID>` hay `maxpack.image`, `p5z/` y `fakelib/`, y los sueltos, no los
    volúmenes. `maxpack.image` es texto: `maxpack-image 1`, `p5z/<nombre>.p5z` y una línea por volumen.
  - Si sale `(zstd)`, `(lz4)` u otro modo, o ninguno, para y guarda el registro.
- [ ] **6.2. Un `.p5z` en un USB.**
  - Pon el `.p5z` en `homebrew/` de un USB (o una carpeta más abajo) y quítalo de `/data/homebrew`.
  - Esperado: arranca, los vídeos se ven sin tirones, cargar y guardar partida funcionan, y 10 minutos
    de juego sin errores. Sales al menú sin cuelgue; en el registro, `cleanup done`.
- [ ] **6.3. Un `.p5z` en la unidad M.2.** Lo mismo desde `homebrew/` de `/mnt/ext1`. Esperado: igual
  que 6.2.
- [ ] **6.4. Un `.p5z` en el SSD interno.**
  - Cópialo a `/data/homebrew/` (no vale otra carpeta de `/data`: es una raíz de escaneo).
  - Esperado: igual que 6.2.
- [ ] **6.5. Moverlo de unidad.**
  - Con la caché ya hecha, mueve el `.p5z` de una unidad a otra (por ejemplo, del USB a
    `/data/homebrew`), con el mismo archivo.
  - Esperado: aviso «<juego> movido a <ruta>», y en el registro `<TID>: moved <ruta vieja> -> <ruta
    nueva>; relinking <caché>`, **sin** `building cache` ni extraer de nuevo; el juego arranca desde
    la unidad nueva y sus partidas y archivos siguen ahí.
- [ ] **6.6. Cambiarlo por otra versión.**
  - Sustituye el `.p5z` por otro del mismo juego (otra versión, o el mismo hecho otra vez).
  - Esperado: en el registro, `building cache` otra vez (la caché se rehace con los sueltos de la
    nueva), y el juego arranca con la versión nueva.
- [ ] **6.7. Instalarlo con la pestaña Instalar.**
  - En Instalar, instala ese `.p5z` en el almacenamiento interno.
  - Esperado: la copia en carpeta gana, el `.p5z` sale como `duplicate` y el juego arranca desde la
    carpeta; la caché pasa a ser la de un juego en carpeta (solo `fakelib/`) y el espacio de los sueltos
    del `.p5z` se libera: compara el espacio libre de `/data` o `/api/state` (`cache.size`) antes y
    después.
  - Desinstálalo (Instalar > Desinstalar). Esperado: el `.p5z` vuelve a ser el juego y arranca.
- [ ] **6.8. Un `.p5z` cortado.**
  - Copia un `.p5z` y quítale el final (por ejemplo, `head -c <la mitad> juego.p5z > cortado.p5z`),
    en una raíz de escaneo.
  - Esperado:
    - no sale en la biblioteca como juego;
    - sale en «Archivos .p5z que no se pueden jugar», con «Dañado o incompleto»; en `/api/state`,
      `unplayable` lo lista con `code` `damaged` y su `detail`;
    - un aviso «No se puede jugar <archivo>: está dañado o incompleto», una sola vez;
    - los demás juegos siguen igual. Borra el archivo al acabar: desaparece de la lista.
- [ ] **6.9. Sin espacio.**
  - Llena `/data` hasta que le quede menos que los sueltos del `.p5z` más 256 MiB, y pon el `.p5z`
    (con la caché borrada: «Purgar caché»).
  - Esperado:
    - el juego sale con «Error» y su ficha dice cuánto falta; `/api/state`: `error_code` `no_space` y
      `missing_bytes`;
    - un aviso «No cabe <juego> en el SSD interno: faltan N GB», una sola vez;
    - en el registro, una línea `... bytes missing; tried again every pass`, una vez por sesión.
  - Libera espacio. Esperado: sin tocar nada, en la siguiente pasada se construye y el juego se monta
    y arranca.
- [ ] **6.10. Salir del juego diez veces.**
  - Lanza el `.p5z` y sal al menú, diez veces.
  - Esperado, cada vez: `overlay MOUNTED` y `cleanup done`, sin `(MNT_FORCE)`, y la consola no se
    cuelga ni se apaga.
- [ ] **6.11. Otro montador en marcha.**
  - Con ShadowMountPlus en marcha, arranca MaxPack con el `.p5z` en una raíz.
  - Esperado: el aviso de «otro montador» y `waiting` = `"other:shadowmountplus.elf"`; el `.p5z` no se
    monta ni se construye (ninguna línea `building cache`, `registered` ni `overlay MOUNTED` suya), y el
    instalador sigue funcionando (Instalar responde).
  - Cierra ShadowMountPlus. Esperado: a los 2-4 s MaxPack vuelve a montar y el `.p5z` sale en el menú.
