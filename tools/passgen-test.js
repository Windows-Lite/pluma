// Test of the strong password generator (src/passgen.js): length, the field's minlength/maxlength/pattern, the kinds of
// characters, no look-alike ones. node tools\passgen-test.js
const fs = require('fs');
const path = require('path');
const src = fs.readFileSync(path.join(__dirname, '..', 'src', 'passgen.js'), 'utf8');
new Function(src)();   // its default config has test: true, which only exposes the generator
const { generate } = globalThis.__plumaPassgenTest;
let bad = 0;
const check = (name, ok) => { if (!ok) { bad++; console.log('FALLA: ' + name); } };
const f = (o) => Object.assign({ minLength: -1, maxLength: -1, pattern: '' }, o);
for (let i = 0; i < 2000; i++) {
  const p = generate(f({}));
  check('15 caracteres: ' + p, p.length === 15);
  check('minúscula, mayúscula, número y símbolo: ' + p, /[a-z]/.test(p) && /[A-Z]/.test(p) && /[0-9]/.test(p) && /[-_.:!]/.test(p));
  check('sin caracteres que se confunden: ' + p, !/[lIO01o]/.test(p));
  check('sin -- ni __: ' + p, !/--|__/.test(p));
  check('solo los permitidos: ' + p, /^[a-km-zA-HJ-NP-Z2-9\-_.:!]+$/.test(p));
}
for (let i = 0; i < 300; i++) {
  check('maxlength 12', generate(f({ maxLength: 12 })).length === 12);
  check('minlength 20', generate(f({ minLength: 20 })).length === 20);
  check('minlength 20 y maxlength 24', generate(f({ minLength: 20, maxLength: 24 })).length === 20);
  check('maxlength 6', generate(f({ maxLength: 6 })).length === 6);
  const n = generate(f({ pattern: '[0-9]{6}' }));
  check('pattern de 6 cifras: ' + n, /^[0-9]{6}$/.test(n));
  const a = generate(f({ pattern: '[A-Za-z0-9]{8,16}' }));
  check('pattern sin símbolos: ' + a, /^[A-Za-z0-9]{8,16}$/.test(a));
  const s = generate(f({ pattern: '(?=.*[!@#]).{12,}', maxLength: 30 }));
  check('pattern que pide un símbolo: ' + s, /^(?=.*[!@#]).{12,}$/.test(s) || s === null);
}
check('nada cabe: maxlength 3', generate(f({ maxLength: 3 })) === null);
check('minlength mayor que maxlength', generate(f({ minLength: 20, maxLength: 10 })) === null);
check('pattern imposible', generate(f({ pattern: 'abc' })) === null);
check('pattern roto: se ignora', generate(f({ pattern: '[' })).length === 15);
console.log(bad ? bad + ' fallos' : 'todo bien');
process.exit(bad ? 1 : 0);
