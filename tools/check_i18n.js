// Fails when a language table in src/ui.html lacks a key the English table has, has an extra
// key, changes the {placeholders}, or still carries the English text. Usage: node tools/check_i18n.js [--list]
const fs = require('fs'), path = require('path'), vm = require('vm');
const html = fs.readFileSync(path.join(__dirname, '..', 'src', 'ui.html'), 'utf8');
const a = html.indexOf('var MSG = {'), b = html.indexOf('\n};', a);
if (a < 0 || b < 0) { console.error('MSG table not found'); process.exit(2); }
const MSG = vm.runInNewContext('(' + html.slice(a + 'var MSG = '.length, b + 2) + ')');
const ph = s => (String(s).match(/\{\w+\}/g) || []).sort().join(',');
// A text of up to 3 words (Version, Status, Title ID, MaxPack .p5z, a unit) may equal the English
// one: it is a name or a cognate. Anything longer identical to English is an untranslated text.
const SAME_OK = new Set();
const en = MSG.en, list = process.argv.includes('--list');
let bad = 0;
for (const lang of Object.keys(MSG)) {
  if (lang === 'en') continue;
  const t = MSG[lang], miss = [], extra = [], same = [], phs = [];
  for (const k of Object.keys(en)) {
    if (!(k in t)) { miss.push(k); continue; }
    if (ph(t[k]) !== ph(en[k])) phs.push(k);
    if (t[k] === en[k] && /[a-z]{4,}/i.test(String(en[k]).replace(/\{\w+\}/g, '')) && String(en[k]).trim().split(/\s+/).length > 3 && !SAME_OK.has(k)) same.push(k);
  }
  for (const k of Object.keys(t)) if (!(k in en)) extra.push(k);
  const n = miss.length + extra.length + phs.length + same.length;
  if (list || n) console.log(lang + ': missing ' + miss.length + (miss.length ? ' [' + miss.join(' ') + ']' : '') +
    ', extra [' + extra.join(' ') + '], placeholder mismatch [' + phs.join(' ') + ']' + ', untranslated (same as EN) [' + same.join(' ') + ']');
  bad += miss.length + extra.length + phs.length + same.length;
}
// The console notices: every language has its own arm; a `_ =>` arm would hide a missing one.
const rs = fs.readFileSync(path.join(__dirname, '..', 'src', 'i18n.rs'), 'utf8');
rs.split('\n').forEach((line, i) => {
  if (/^\s*_\s*=>\s*format!|^\s*_\s*=>\s*"/.test(line)) { console.log('src/i18n.rs:' + (i + 1) + ': a text falls back with `_ =>`: give each language its own arm'); bad++; }
});
const langs = html.match(/var LANGS = \{([\s\S]*?)\n\};/)[1].match(/\b[a-z]{2}(?=: \{ name)/g);
for (const l of Object.keys(MSG)) if (!langs.includes(l)) { console.log('LANGS lacks ' + l); bad++; }
console.log(bad ? 'FAIL: ' + bad + ' problem(s)' : 'ok: ' + Object.keys(MSG).length + ' languages, ' + Object.keys(en).length + ' keys each');
process.exit(bad ? 1 : 0);
