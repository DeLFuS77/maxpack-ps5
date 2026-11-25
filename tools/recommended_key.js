// The key of a title id in data/recommended.json (maxpack_logic::recommended::key): the first 16 bytes
// of the SHA-256 of "maxpack-recommended-v1:" and the title id, in lowercase hex. The list names no
// game, so it is maintained with this. Usage: node tools/recommended_key.js <TITLE_ID>
const crypto = require('crypto');
const tid = process.argv[2] || '';
// As maxpack_logic::titles::valid_tid: four upper-case letters and five digits, never a system app.
if (process.argv.length !== 3 || !/^[A-Z]{4}[0-9]{5}$/.test(tid) || tid.startsWith('NPXS')) {
  console.error('usage: node tools/recommended_key.js <TITLE_ID> (four upper-case letters and five digits, like PPSA01234)');
  process.exit(2);
}
console.log(crypto.createHash('sha256').update('maxpack-recommended-v1:' + tid, 'ascii').digest('hex').slice(0, 32));
