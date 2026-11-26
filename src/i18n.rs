//! Toast texts in the console's language (System Settings > Language), as ps5-dump-forge's
//! notice.rs: the same 11 languages, English when the language can't be read or has no translation. "MaxPack"
//! stays untranslated; arguments (title names, sizes, paths) are kept as given.

pub use maxpack_logic::lang::Lang;

extern "C" {
    /// libSceSystemService: a system setting by id.
    fn sceSystemServiceParamGetInt(param_id: i32, value: *mut i32) -> i32;
}

/// The console's language now (read per toast, so a change applies without a restart);
/// English when it can't be read or has no translation (`Lang::from_console`).
pub fn lang() -> Lang {
    console_code().map_or(Lang::En, Lang::from_console)
}

/// `SCE_SYSTEM_PARAM_LANG_*` of the console now, None when it can't be read.
fn console_code() -> Option<i32> {
    /// `SCE_SYSTEM_SERVICE_PARAM_ID_LANG`.
    const LANG: i32 = 1;
    let mut code = -1;
    // SAFETY: one i32 out-parameter that outlives the call.
    (unsafe { sceSystemServiceParamGetInt(LANG, &mut code) } == 0).then_some(code)
}

/// param.json `localizedParameters` tags by `SCE_SYSTEM_PARAM_LANG_*` (SMP sm_l10n.c order, the
/// tags LibProsperoPkg writes).
const TITLE_TAGS: [&str; 31] = [
    "ja-JP", "en-US", "fr-FR", "es-ES", "de-DE", "it-IT", "nl-NL", "pt-PT", "ru-RU", "ko-KR",
    "zh-Hant", "zh-Hans", "fi-FI", "sv-SE", "da-DK", "no-NO", "pl-PL", "pt-BR", "en-GB", "tr-TR",
    "es-419", "ar-AE", "fr-CA", "cs-CZ", "hu-HU", "el-GR", "ro-RO", "th-TH", "vi-VN", "id-ID",
    "uk-UA",
];

/// The console language's `localizedParameters` tag (all 31 console languages, not only the 11
/// toast languages); en-US when it can't be read.
pub fn title_tag() -> &'static str {
    console_code().and_then(|c| TITLE_TAGS.get(usize::try_from(c).ok()?)).copied().unwrap_or("en-US")
}

/// The payload is up and serving the web UI.
pub fn started(l: Lang) -> &'static str {
    match l {
        Lang::En => "MaxPack started",
        Lang::Ru => "MaxPack запущен",
        Lang::Zh => "MaxPack 已启动",
        Lang::De => "MaxPack wurde gestartet",
        Lang::Es => "MaxPack se ha iniciado",
        Lang::Fr => "MaxPack est lancé",
        Lang::Ja => "MaxPack を起動しました",
        Lang::Pt => "O MaxPack foi iniciado",
        Lang::It => "MaxPack è stato avviato",
        Lang::Ko => "MaxPack가 실행되었습니다",
        Lang::Ar => "تم تشغيل MaxPack",
    }
}

/// Shut down from the web UI.
pub fn stopped(l: Lang) -> &'static str {
    match l {
        Lang::En => "MaxPack stopped",
        Lang::Ru => "MaxPack остановлен",
        Lang::Zh => "MaxPack 已停止",
        Lang::De => "MaxPack wurde beendet",
        Lang::Es => "MaxPack se ha detenido",
        Lang::Fr => "MaxPack est arrêté",
        Lang::Ja => "MaxPack は停止しました",
        Lang::Pt => "O MaxPack foi parado",
        Lang::It => "MaxPack è stato arrestato",
        Lang::Ko => "MaxPack가 중지되었습니다",
        Lang::Ar => "توقّف MaxPack",
    }
}

/// The web UI port is taken: the phase-2 maxpack.elf (it has no quit for us to ask), an older copy
/// with a game running, or another app.
pub fn port_busy(l: Lang, port: u16) -> String {
    match l {
        // The phase-2 maxpack.elf has the same name and port but no quit for a newer copy to ask
        // (web::take_port): it is the likely case, so English and Spanish name it.
        Lang::En => format!("MaxPack: port {port} busy, not started. Is the earlier MaxPack (the phase-2 installer) still running? Restart the console. Or an older copy with a game open, or another app"),
        Lang::Ru => format!("MaxPack: порт {port} занят (старая копия с запущенной игрой или другое приложение?); не запущен"),
        Lang::Zh => format!("MaxPack：端口 {port} 已被占用（正在运行游戏的旧版本，或其他应用？）；未启动"),
        Lang::De => format!("MaxPack: Port {port} belegt (eine ältere Kopie mit laufendem Spiel oder eine andere App?); nicht gestartet"),
        Lang::Es => format!("MaxPack: puerto {port} ocupado, no se ha iniciado. ¿Sigue en marcha el MaxPack anterior (instalador de la fase 2)? Reinicia la consola. O una copia anterior con un juego abierto, u otra aplicación"),
        Lang::Fr => format!("MaxPack\u{202f}: le port {port} est occupé (une ancienne copie avec un jeu en cours, ou une autre application\u{202f}?)\u{202f}; non lancé"),
        Lang::Ja => format!("MaxPack：ポート {port} は使用中です（ゲーム実行中の古いコピー、または別のアプリ？）。起動しませんでした"),
        Lang::Pt => format!("MaxPack: a porta {port} está ocupada (uma cópia antiga com um jogo em execução ou outro aplicativo?); não foi iniciado"),
        Lang::It => format!("MaxPack: la porta {port} è occupata (una copia precedente con un gioco in corso o un'altra app?); non avviato"),
        Lang::Ko => format!("MaxPack: 포트 {port}이(가) 사용 중입니다(게임이 실행 중인 이전 사본 또는 다른 앱?). 시작하지 않았습니다"),
        Lang::Ar => format!("MaxPack: المنفذ {port} مشغول (نسخة أقدم تشغّل لعبة، أو تطبيق آخر؟)؛ لم يتم التشغيل"),
    }
}

/// The running copy did not hand the port over because it is finishing a build (web::take_port's
/// PortTaken::Building): sending this one again in a moment works.
pub fn finishing_build(l: Lang) -> &'static str {
    match l {
        Lang::En => "MaxPack is finishing a build, so this copy did not start. Try again in a moment",
        Lang::Ru => "MaxPack завершает сборку, поэтому эта копия не запущена. Повторите через минуту",
        Lang::Zh => "MaxPack 正在完成一次构建，因此这个副本没有启动。请稍后再试",
        Lang::De => "MaxPack schließt gerade einen Build ab, daher wurde diese Kopie nicht gestartet. Versuche es gleich noch einmal",
        Lang::Es => "MaxPack está terminando de construir una caché, así que esta copia no se ha iniciado. Vuelve a probar en un momento",
        Lang::Fr => "MaxPack termine une construction, cette copie n'a donc pas été lancée. Réessayez dans un instant",
        Lang::Ja => "MaxPack がビルドを終えるところのため、このコピーは起動しませんでした。少し待ってからもう一度お試しください",
        Lang::Pt => "O MaxPack está terminando uma construção, por isso esta cópia não foi iniciada. Tente de novo daqui a pouco",
        Lang::It => "MaxPack sta finendo una build, quindi questa copia non è stata avviata. Riprova tra un momento",
        Lang::Ko => "MaxPack이 빌드를 마무리하는 중이라 이 사본은 시작되지 않았습니다. 잠시 후 다시 시도하세요",
        Lang::Ar => "يُنهي MaxPack عملية بناء، لذلك لم تبدأ هذه النسخة. حاول مرة أخرى بعد قليل",
    }
}

/// `/data` free space is below `low_space_gb`.
pub fn low_space(l: Lang, gb: u64) -> String {
    match l {
        Lang::En => format!("MaxPack: /data has only {gb} GB free"),
        Lang::Ru => format!("MaxPack: на /data свободно всего {gb} ГБ"),
        Lang::Zh => format!("MaxPack：/data 仅剩 {gb} GB 可用空间"),
        Lang::De => format!("MaxPack: /data hat nur noch {gb} GB frei"),
        Lang::Es => format!("MaxPack: /data solo tiene {gb} GB libres"),
        Lang::Fr => format!("MaxPack\u{202f}: /data n’a plus que {gb} Go libres"),
        Lang::Ja => format!("MaxPack：/data の空き容量は {gb} GB のみです"),
        Lang::Pt => format!("MaxPack: /data tem apenas {gb} GB livres"),
        Lang::It => format!("MaxPack: /data ha solo {gb} GB liberi"),
        Lang::Ko => format!("MaxPack: /data의 여유 공간이 {gb}GB밖에 없습니다"),
        Lang::Ar => format!("MaxPack: لا يتوفر في /data سوى {gb} غيغابايت"),
    }
}

/// A .p5z's loose files and the margin do not fit on the internal SSD: `missing` bytes short.
pub fn no_space(l: Lang, name: &str, missing: u64) -> String {
    // The decimal mark of each language (a comma where the language writes one).
    let comma = matches!(l, Lang::Ru | Lang::De | Lang::Es | Lang::Fr | Lang::Pt | Lang::It);
    let gb = gb_up(missing, if comma { ',' } else { '.' });
    match l {
        Lang::En => format!("{name} does not fit on the internal SSD: {gb} GB missing"),
        Lang::Ru => format!("{name} не помещается на внутренний SSD: не хватает {gb} ГБ"),
        Lang::Zh => format!("{name} 无法放入内部 SSD：还差 {gb} GB"),
        Lang::De => format!("{name} passt nicht auf die interne SSD: {gb} GB fehlen"),
        Lang::Es => format!("No cabe {name} en el SSD interno: faltan {gb} GB"),
        Lang::Fr => format!("{name} ne tient pas sur le SSD interne\u{202f}: il manque {gb} Go"),
        Lang::Ja => format!("{name} は内蔵 SSD に入りません：あと {gb} GB 必要です"),
        Lang::Pt => format!("{name} não cabe no SSD interno: faltam {gb} GB"),
        Lang::It => format!("{name} non entra nell'SSD interno: mancano {gb} GB"),
        Lang::Ko => format!("{name}이(가) 내장 SSD에 들어가지 않습니다: {gb}GB 부족"),
        Lang::Ar => format!("لا يتسع SSD الداخلي لـ {name}: ينقص {gb} غيغابايت"),
    }
}

/// A .p5z that cannot be played (damaged or incomplete, or too many volumes).
pub fn p5z_unplayable(l: Lang, file_name: &str) -> String {
    match l {
        Lang::En => format!("{file_name} cannot be played: it is damaged or incomplete"),
        Lang::Ru => format!("{file_name}: игру нельзя запустить, файл повреждён или неполный"),
        Lang::Zh => format!("无法运行 {file_name}：文件已损坏或不完整"),
        Lang::De => format!("{file_name} kann nicht gespielt werden: beschädigt oder unvollständig"),
        Lang::Es => format!("No se puede jugar {file_name}: está dañado o incompleto"),
        Lang::Fr => format!("{file_name} ne peut pas être lancé\u{202f}: endommagé ou incomplet"),
        Lang::Ja => format!("{file_name} をプレイできません：破損しているか不完全です"),
        Lang::Pt => format!("Não é possível jogar {file_name}: está danificado ou incompleto"),
        Lang::It => format!("{file_name} non si può giocare: è danneggiato o incompleto"),
        Lang::Ko => format!("{file_name}을(를) 플레이할 수 없습니다: 손상되었거나 불완전합니다"),
        Lang::Ar => format!("تعذّر تشغيل {file_name}: الملف تالف أو غير مكتمل"),
    }
}

/// A .p5z whose index is of a version this MaxPack does not read (maxpack_logic::p5z::Unplayable::Newer): made by a
/// newer MaxPack for the PC; never "damaged".
pub fn p5z_newer(l: Lang, file_name: &str) -> String {
    match l {
        Lang::En => format!("{file_name} was made by a newer MaxPack for the PC: update MaxPack for the console to play it"),
        Lang::Ru => format!("{file_name} создан более новой версией MaxPack для ПК: обновите MaxPack на консоли, чтобы играть в него"),
        Lang::Zh => format!("{file_name} 由更新版本的电脑版 MaxPack 制作：请更新主机上的 MaxPack 后再运行"),
        Lang::De => format!("{file_name} stammt von einem neueren MaxPack für den PC: Aktualisiere MaxPack auf der Konsole, um es zu spielen"),
        Lang::Es => format!("{file_name} lo hizo un MaxPack para PC más nuevo: actualiza MaxPack en la consola para jugarlo"),
        Lang::Fr => format!("{file_name} a été créé par un MaxPack pour PC plus récent\u{202f}: mettez à jour MaxPack sur la console pour y jouer"),
        Lang::Ja => format!("{file_name} は新しいバージョンの PC 版 MaxPack で作られています：プレイするにはコンソールの MaxPack を更新してください"),
        Lang::Pt => format!("{file_name} foi feito por um MaxPack para PC mais novo: atualize o MaxPack no console para jogá-lo"),
        Lang::It => format!("{file_name} è stato creato da un MaxPack per PC più recente: aggiorna MaxPack sulla console per giocarci"),
        Lang::Ko => format!("{file_name}은(는) 더 새로운 PC용 MaxPack으로 만들어졌습니다: 플레이하려면 콘솔의 MaxPack을 업데이트하세요"),
        Lang::Ar => format!("أُنشئ {file_name} بإصدار أحدث من MaxPack للحاسوب: حدّث MaxPack على الجهاز لتشغيله"),
    }
}

/// `bytes` in GB (2^30) with one decimal and `mark` between, rounded up: a shortage never reads 0.0.
fn gb_up(bytes: u64, mark: char) -> String {
    let tenths = (u128::from(bytes) * 10).div_ceil(1 << 30);
    format!("{}{mark}{}", tenths / 10, tenths % 10)
}

/// A data directory can't be created at start-up; `err` is the OS error (English).
pub fn cannot_create(l: Lang, dir: &str, err: &str) -> String {
    match l {
        Lang::En => format!("MaxPack: cannot create {dir}: {err}"),
        Lang::Ru => format!("MaxPack: не удаётся создать {dir}: {err}"),
        Lang::Zh => format!("MaxPack：无法创建 {dir}：{err}"),
        Lang::De => format!("MaxPack: {dir} kann nicht erstellt werden: {err}"),
        Lang::Es => format!("MaxPack: no se puede crear {dir}: {err}"),
        Lang::Fr => format!("MaxPack\u{202f}: impossible de créer {dir}\u{202f}: {err}"),
        Lang::Ja => format!("MaxPack：{dir} を作成できません：{err}"),
        Lang::Pt => format!("MaxPack: não é possível criar {dir}: {err}"),
        Lang::It => format!("MaxPack: impossibile creare {dir}: {err}"),
        Lang::Ko => format!("MaxPack: {dir}을(를) 만들 수 없습니다: {err}"),
        Lang::Ar => format!("MaxPack: تعذّر إنشاء {dir}: {err}"),
    }
}

/// A game was newly registered on the home screen.
pub fn registered(l: Lang, name: &str) -> String {
    match l {
        Lang::En => format!("{name} registered"),
        Lang::Ru => format!("{name}: игра зарегистрирована"),
        Lang::Zh => format!("{name} 已注册"),
        Lang::De => format!("{name} registriert"),
        Lang::Es => format!("{name} registrado"),
        Lang::Fr => format!("{name} enregistré"),
        Lang::Ja => format!("{name} を登録しました"),
        Lang::Pt => format!("{name} registrado"),
        Lang::It => format!("{name} registrato"),
        Lang::Ko => format!("{name} 등록됨"),
        Lang::Ar => format!("تم تسجيل {name}"),
    }
}

/// `n` games registered by one install call (FW 12+ batch): one toast instead of one per game.
pub fn registered_many(l: Lang, n: usize) -> String {
    match l {
        Lang::En => format!("{n} games registered"),
        Lang::Ru => format!("Зарегистрировано игр: {n}"),
        Lang::Zh => format!("已注册 {n} 款游戏"),
        Lang::De => format!("{n} Spiele registriert"),
        Lang::Es => format!("{n} juegos registrados"),
        Lang::Fr => format!("{n} jeux enregistrés"),
        Lang::Ja => format!("{n} 本のゲームを登録しました"),
        Lang::Pt => format!("{n} jogos registrados"),
        Lang::It => format!("{n} giochi registrati"),
        Lang::Ko => format!("게임 {n}개 등록됨"),
        Lang::Ar => format!("تم تسجيل الألعاب: {n}"),
    }
}

/// A newer ampr-emu-ng release (`version`, its tag) was downloaded and installed.
pub fn ampr_updated(l: Lang, version: &str) -> String {
    match l {
        Lang::En => format!("ampr-emu-ng {version} installed"),
        Lang::Ru => format!("ampr-emu-ng {version} установлен"),
        Lang::Zh => format!("ampr-emu-ng {version} 已安装"),
        Lang::De => format!("ampr-emu-ng {version} installiert"),
        Lang::Es => format!("ampr-emu-ng {version} instalado"),
        Lang::Fr => format!("ampr-emu-ng {version} installé"),
        Lang::Ja => format!("ampr-emu-ng {version} をインストールしました"),
        Lang::Pt => format!("ampr-emu-ng {version} instalado"),
        Lang::It => format!("ampr-emu-ng {version} installato"),
        Lang::Ko => format!("ampr-emu-ng {version} 설치됨"),
        Lang::Ar => format!("تم تثبيت ampr-emu-ng {version}"),
    }
}

/// An ampr-emu-ng release (`version`, its tag) was downloaded but not installed: its SHA256SUMS is
/// missing or malformed, or the download is not the libSceAmpr.sprx it lists (maxpack_logic::ampr).
pub fn ampr_refused(l: Lang, version: &str) -> String {
    match l {
        Lang::En => format!("ampr-emu-ng {version} not installed: it does not match the release's SHA256SUMS"),
        Lang::Ru => format!("ampr-emu-ng {version} не установлен: не совпадает с SHA256SUMS релиза"),
        Lang::Zh => format!("未安装 ampr-emu-ng {version}：与该版本的 SHA256SUMS 不符"),
        Lang::De => format!("ampr-emu-ng {version} nicht installiert: passt nicht zu den SHA256SUMS des Releases"),
        Lang::Es => format!("ampr-emu-ng {version} no instalado: no coincide con el SHA256SUMS de la versión"),
        Lang::Fr => format!("ampr-emu-ng {version} non installé : ne correspond pas au SHA256SUMS de la version"),
        Lang::Ja => format!("ampr-emu-ng {version} はインストールしませんでした：リリースの SHA256SUMS と一致しません"),
        Lang::Pt => format!("ampr-emu-ng {version} não instalado: não confere com o SHA256SUMS da versão"),
        Lang::It => format!("ampr-emu-ng {version} non installato: non corrisponde allo SHA256SUMS della release"),
        Lang::Ko => format!("ampr-emu-ng {version} 설치 안 됨: 릴리스의 SHA256SUMS와 일치하지 않음"),
        Lang::Ar => format!("لم يُثبَّت ampr-emu-ng {version}: لا يطابق SHA256SUMS الخاص بالإصدار"),
    }
}

/// A game moved to another drive or folder (`path`, the new source) was relinked, no rebuild.
pub fn moved(l: Lang, name: &str, path: &str) -> String {
    match l {
        Lang::En => format!("{name} moved to {path}"),
        Lang::Ru => format!("{name}: игра перемещена в {path}"),
        Lang::Zh => format!("{name} 已移动到 {path}"),
        Lang::De => format!("{name} nach {path} verschoben"),
        Lang::Es => format!("{name} movido a {path}"),
        Lang::Fr => format!("{name} déplacé vers {path}"),
        Lang::Ja => format!("{name} を {path} に移動しました"),
        Lang::Pt => format!("{name} movido para {path}"),
        Lang::It => format!("{name} spostato in {path}"),
        Lang::Ko => format!("{name}이(가) {path}(으)로 이동됨"),
        Lang::Ar => format!("تم نقل {name} إلى {path}"),
    }
}

/// A game's build or registration failed; the error is in maxpack.log and the UI.
pub fn failed(l: Lang, name: &str) -> String {
    match l {
        Lang::En => format!("{name}: failed, see the log"),
        Lang::Ru => format!("{name}: ошибка, см. журнал"),
        Lang::Zh => format!("{name}：失败，请查看日志"),
        Lang::De => format!("{name}: fehlgeschlagen, siehe Protokoll"),
        Lang::Es => format!("{name}: error, consulta el registro"),
        Lang::Fr => format!("{name}\u{202f}: échec, voir le journal"),
        Lang::Ja => format!("{name}：失敗しました。ログを確認してください"),
        Lang::Pt => format!("{name}: falhou, veja o log"),
        Lang::It => format!("{name}: non riuscito, vedi il log"),
        Lang::Ko => format!("{name}: 실패했습니다. 로그를 확인하세요"),
        Lang::Ar => format!("{name}: فشل، راجع السجل"),
    }
}

/// A .p5z that grew is not started: the ampr-emu-ng in /data/maxpack/ampr/ is older than the one it
/// needs, or of unknown version (cache.rs OldNg); the game's sheet says what to do.
pub fn old_ng(l: Lang, name: &str) -> String {
    match l {
        Lang::En => format!("{name} needs a newer ampr-emu-ng: see the game on MaxPack's page"),
        Lang::Ru => format!("{name}: нужен более новый ampr-emu-ng, подробности на странице MaxPack"),
        Lang::Zh => format!("{name} 需要更新的 ampr-emu-ng：请在 MaxPack 页面查看该游戏"),
        Lang::De => format!("{name} braucht ein neueres ampr-emu-ng: siehe das Spiel auf der Seite von MaxPack"),
        Lang::Es => format!("{name} necesita un ampr-emu-ng más nuevo: mira el juego en la página de MaxPack"),
        Lang::Fr => format!("{name} demande un ampr-emu-ng plus récent\u{202f}: voir le jeu sur la page de MaxPack"),
        Lang::Ja => format!("{name} には新しい ampr-emu-ng が必要です：MaxPack のページでゲームを確認してください"),
        Lang::Pt => format!("{name} precisa de um ampr-emu-ng mais novo: veja o jogo na página do MaxPack"),
        Lang::It => format!("{name} richiede un ampr-emu-ng più recente: vedi il gioco nella pagina di MaxPack"),
        Lang::Ko => format!("{name}에는 더 새로운 ampr-emu-ng가 필요합니다: MaxPack 페이지에서 게임을 확인하세요"),
        Lang::Ar => format!("يحتاج {name} إلى ampr-emu-ng أحدث: راجع اللعبة في صفحة MaxPack"),
    }
}

/// `old_ng` for `n` games at once (an older ampr-emu-ng stops every .p5z that grew): one toast, as
/// `failed_many`, instead of one per game.
pub fn old_ng_many(l: Lang, n: usize) -> String {
    match l {
        Lang::En => format!("{n} games need a newer ampr-emu-ng: see them on MaxPack's page"),
        Lang::Ru => format!("Нужен более новый ampr-emu-ng играм: {n}, подробности на странице MaxPack"),
        Lang::Zh => format!("{n} 款游戏需要更新的 ampr-emu-ng：请在 MaxPack 页面查看这些游戏"),
        Lang::De => format!("{n} Spiele brauchen ein neueres ampr-emu-ng: siehe die Spiele auf der Seite von MaxPack"),
        Lang::Es => format!("{n} juegos necesitan un ampr-emu-ng más nuevo: míralos en la página de MaxPack"),
        Lang::Fr => format!("{n} jeux demandent un ampr-emu-ng plus récent\u{202f}: voir les jeux sur la page de MaxPack"),
        Lang::Ja => format!("{n} 本のゲームには新しい ampr-emu-ng が必要です：MaxPack のページでゲームを確認してください"),
        Lang::Pt => format!("{n} jogos precisam de um ampr-emu-ng mais novo: veja os jogos na página do MaxPack"),
        Lang::It => format!("{n} giochi richiedono un ampr-emu-ng più recente: vedi i giochi nella pagina di MaxPack"),
        Lang::Ko => format!("게임 {n}개에 더 새로운 ampr-emu-ng가 필요합니다: MaxPack 페이지에서 게임을 확인하세요"),
        Lang::Ar => format!("ألعاب تحتاج إلى ampr-emu-ng أحدث: {n}، راجعها في صفحة MaxPack"),
    }
}

/// The install call for a batch of `n` games failed; each error is in maxpack.log and the UI.
pub fn failed_many(l: Lang, n: usize) -> String {
    match l {
        Lang::En => format!("{n} games failed, see the log"),
        Lang::Ru => format!("Ошибка у игр: {n}, см. журнал"),
        Lang::Zh => format!("{n} 款游戏失败，请查看日志"),
        Lang::De => format!("{n} Spiele fehlgeschlagen, siehe Protokoll"),
        Lang::Es => format!("{n} juegos con error, consulta el registro"),
        Lang::Fr => format!("{n} jeux en échec, voir le journal"),
        Lang::Ja => format!("{n} 本のゲームが失敗しました。ログを確認してください"),
        Lang::Pt => format!("{n} jogos falharam, veja o log"),
        Lang::It => format!("{n} giochi non riusciti, vedi il log"),
        Lang::Ko => format!("게임 {n}개 실패. 로그를 확인하세요"),
        Lang::Ar => format!("فشلت الألعاب: {n}، راجع السجل"),
    }
}

/// A game's source and cache were deleted from the web UI.
pub fn deleted(l: Lang, name: &str) -> String {
    match l {
        Lang::En => format!("{name} deleted"),
        Lang::Ru => format!("{name}: игра удалена"),
        Lang::Zh => format!("{name} 已删除"),
        Lang::De => format!("{name} gelöscht"),
        Lang::Es => format!("{name} eliminado"),
        Lang::Fr => format!("{name} supprimé"),
        Lang::Ja => format!("{name} を削除しました"),
        Lang::Pt => format!("{name} excluído"),
        Lang::It => format!("{name} eliminato"),
        Lang::Ko => format!("{name} 삭제됨"),
        Lang::Ar => format!("تم حذف {name}"),
    }
}

/// kstuff does not answer its readiness probe: start-up waits for it (SMP SM_L10N_KSTUFF_WAITING).
pub fn kstuff_waiting(l: Lang) -> &'static str {
    match l {
        Lang::En => "MaxPack: kstuff not found; waiting for it to start",
        Lang::Ru => "MaxPack: kstuff не найден; ожидание его запуска",
        Lang::Zh => "MaxPack：未找到 kstuff，正在等待其启动",
        Lang::De => "MaxPack: kstuff nicht gefunden; warte auf den Start",
        Lang::Es => "MaxPack: no se encontró kstuff; esperando a que se inicie",
        Lang::Fr => "MaxPack : kstuff introuvable ; en attente de son démarrage",
        Lang::Ja => "MaxPack: kstuff が見つかりません。起動を待っています",
        Lang::Pt => "MaxPack: kstuff não encontrado; aguardando sua inicialização",
        Lang::It => "MaxPack: kstuff non trovato; in attesa del suo avvio",
        Lang::Ko => "MaxPack: kstuff를 찾을 수 없습니다. 시작될 때까지 기다리는 중입니다",
        Lang::Ar => "MaxPack: لم يتم العثور على kstuff؛ جارٍ انتظار تشغيله",
    }
}

/// Another mounter (`name`, maxpack_logic::mounter::display_name) runs: MaxPack mounts nothing new
/// until it stops.
pub fn other_mounter(l: Lang, name: &str) -> String {
    match l {
        Lang::En => format!("MaxPack: {name} is running; MaxPack mounts no games until it stops"),
        Lang::Ru => format!("MaxPack: запущен {name}; MaxPack не монтирует игры, пока он не остановится"),
        Lang::Zh => format!("MaxPack：{name} 正在运行；在其停止前 MaxPack 不会挂载游戏"),
        Lang::De => format!("MaxPack: {name} läuft; MaxPack bindet keine Spiele ein, bis es beendet ist"),
        Lang::Es => format!("MaxPack: {name} está en marcha; MaxPack no monta juegos hasta que se cierre"),
        Lang::Fr => format!("MaxPack\u{202f}: {name} est en cours d'exécution\u{202f}; MaxPack ne monte aucun jeu tant qu'il n'est pas arrêté"),
        Lang::Ja => format!("MaxPack：{name} が実行中です。停止するまで MaxPack はゲームをマウントしません"),
        Lang::Pt => format!("MaxPack: {name} está em execução; o MaxPack não monta jogos até ele parar"),
        Lang::It => format!("MaxPack: {name} è in esecuzione; MaxPack non monta giochi finché non si arresta"),
        Lang::Ko => format!("MaxPack: {name}이(가) 실행 중입니다. 종료될 때까지 MaxPack은 게임을 마운트하지 않습니다"),
        Lang::Ar => format!("MaxPack: {name} قيد التشغيل؛ لن يركّب MaxPack أي لعبة حتى يتوقف"),
    }
}

/// The other mounter (`name`) stopped: MaxPack mounts again.
pub fn mounting_resumed(l: Lang, name: &str) -> String {
    match l {
        Lang::En => format!("MaxPack: {name} stopped; MaxPack mounts games again"),
        Lang::Ru => format!("MaxPack: {name} остановлен; MaxPack снова монтирует игры"),
        Lang::Zh => format!("MaxPack：{name} 已停止；MaxPack 恢复挂载游戏"),
        Lang::De => format!("MaxPack: {name} wurde beendet; MaxPack bindet wieder Spiele ein"),
        Lang::Es => format!("MaxPack: {name} se ha cerrado; MaxPack vuelve a montar juegos"),
        Lang::Fr => format!("MaxPack\u{202f}: {name} est arrêté\u{202f}; MaxPack monte de nouveau les jeux"),
        Lang::Ja => format!("MaxPack：{name} が停止しました。MaxPack はゲームのマウントを再開します"),
        Lang::Pt => format!("MaxPack: o {name} parou; o MaxPack volta a montar jogos"),
        Lang::It => format!("MaxPack: {name} si è arrestato; MaxPack torna a montare i giochi"),
        Lang::Ko => format!("MaxPack: {name}이(가) 종료되었습니다. MaxPack이 게임 마운트를 다시 시작합니다"),
        Lang::Ar => format!("MaxPack: توقّف {name}؛ يعود MaxPack إلى تركيب الألعاب"),
    }
}

/// ShadowMount+ is running (it mounts the same titles): MaxPack waits for it to stop to mount.
pub fn smp_running(l: Lang) -> &'static str {
    match l {
        Lang::En => "MaxPack: ShadowMount+ is running; waiting for it to stop",
        Lang::Ru => "MaxPack: запущен ShadowMount+; ожидание его остановки",
        Lang::Zh => "MaxPack：ShadowMount+ 正在运行，正在等待其停止",
        Lang::De => "MaxPack: ShadowMount+ läuft; warte, bis es beendet wird",
        Lang::Es => "MaxPack: ShadowMount+ está en ejecución; esperando a que se detenga",
        Lang::Fr => "MaxPack : ShadowMount+ est en cours d'exécution ; en attente de son arrêt",
        Lang::Ja => "MaxPack: ShadowMount+ が実行中です。停止を待っています",
        Lang::Pt => "MaxPack: o ShadowMount+ está em execução; aguardando que ele pare",
        Lang::It => "MaxPack: ShadowMount+ è in esecuzione; in attesa che si arresti",
        Lang::Ko => "MaxPack: ShadowMount+가 실행 중입니다. 종료될 때까지 기다리는 중입니다",
        Lang::Ar => "MaxPack: ShadowMount+ قيد التشغيل؛ جارٍ انتظار إيقافه",
    }
}

/// The recommended settings' notice `long_first_boot` (maxpack_logic::recommended::Notice), shown when
/// such a game starts: its screen stays black for minutes, which looks like a hang.
pub fn notice_long_first_boot(l: Lang) -> &'static str {
    match l {
        Lang::En => "It can take several minutes to start, with a black screen. Wait.",
        Lang::Ru => "Запуск может занять несколько минут, с чёрным экраном. Подождите.",
        Lang::Zh => "启动可能需要几分钟，期间屏幕保持黑色。请稍候。",
        Lang::De => "Der Start kann mehrere Minuten dauern, mit schwarzem Bildschirm. Bitte warten.",
        Lang::Es => "Puede tardar varios minutos en arrancar, con la pantalla en negro. Espera.",
        Lang::Fr => "Le démarrage peut prendre plusieurs minutes, avec un écran noir. Patientez.",
        Lang::Ja => "起動に数分かかることがあり、その間画面は真っ暗です。お待ちください。",
        Lang::Pt => "Pode levar vários minutos para iniciar, com a tela preta. Aguarde.",
        Lang::It => "L'avvio può richiedere diversi minuti, con lo schermo nero. Attendi.",
        Lang::Ko => "시작하는 데 몇 분이 걸릴 수 있으며 그동안 화면이 검게 보입니다. 기다려 주세요.",
        Lang::Ar => "قد يستغرق بدء التشغيل عدة دقائق مع شاشة سوداء. انتظر.",
    }
}
