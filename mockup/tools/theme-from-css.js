// Генерирует mockup/theme.json из mockup/index.html:
//   skins    - блоки материалов (theme:begin..theme:end, :root[data-skin=...])
//   layouts  - блоки раскладок (:root[data-layout=...])
//   settings - схема настроек из <script id="pm-schema"> (вкладки, опции, диапазоны)
// Запуск: node tools/theme-from-css.js
// Цвета приводятся к #RRGGBBAA, px и ms - к числам, тени и кривые - к структурам.
// Значения с var()/color-mix() остаются строками: они зависят от акцента.
const fs = require('fs');
const path = require('path');

const dir = path.join(__dirname, '..');
const html = fs.readFileSync(path.join(dir, 'index.html'), 'utf8');
const block = html.slice(html.indexOf('/* theme:begin */'), html.indexOf('/* theme:end */'));

const GROUPS = {
  c: 'color', r: 'radius', sh: 'shadow', sp: 'spacing', fs: 'font', fw: 'font', ls: 'font', tt: 'font',
  sz: 'size', dur: 'duration', ease: 'easing', spring: 'spring', fx: 'effect', viz: 'visualizer', mq: 'marquee',
  opt: 'defaults',
};

const hex2 = (n) => Math.round(n).toString(16).padStart(2, '0').toUpperCase();
function color(v) {
  let m = v.match(/^#([0-9a-f]{3}|[0-9a-f]{6})$/i);
  if (m) {
    let h = m[1];
    if (h.length === 3) h = h.split('').map((c) => c + c).join('');
    return '#' + h.toUpperCase() + 'FF';
  }
  m = v.match(/^rgba?\(([^)]+)\)$/);
  if (m) {
    const [r, g, b, a = 1] = m[1].split(',').map((x) => parseFloat(x));
    return '#' + hex2(r) + hex2(g) + hex2(b) + hex2(a * 255);
  }
  return null;
}
function shadows(v) {
  // Делим по запятым верхнего уровня (внутри rgba() запятые не трогаем)
  const parts = [];
  let depth = 0, cur = '';
  for (const ch of v) {
    if (ch === '(') depth++;
    if (ch === ')') depth--;
    if (ch === ',' && depth === 0) { parts.push(cur.trim()); cur = ''; } else cur += ch;
  }
  parts.push(cur.trim());
  return parts.map((p) => {
    const inset = /\binset\b/.test(p);
    const col = p.match(/(rgba?\([^)]+\)|#[0-9a-f]{3,8})/i);
    const nums = p.replace(/\binset\b/, '').replace(col ? col[0] : '', '').trim().split(/\s+/).map(parseFloat);
    const [x = 0, y = 0, blur = 0, spread = 0] = nums;
    return { x, y, blur, spread, color: col ? color(col[0]) : '#000000FF', inset };
  });
}
function value(group, v) {
  if (/var\(|color-mix\(/.test(v)) return v;
  if (group === 'shadow') return shadows(v);
  if (group === 'easing') {
    let m = v.match(/^cubic-bezier\(([^)]+)\)$/);
    if (m) return { type: 'cubic-bezier', points: m[1].split(',').map(Number) };
    m = v.match(/^linear\(([^)]+)\)$/);
    if (m) return { type: 'sampled', note: 'выборка пружины spring.*, равномерно по времени', samples: m[1].split(',').map(Number) };
  }
  const c = color(v);
  if (c) return c;
  if (/^-?[\d.]+(px|ms)$/.test(v)) return parseFloat(v);
  if (/^-?[\d.]+$/.test(v)) return parseFloat(v);
  return v;
}

const schema = JSON.parse(html.match(/<script type="application\/json" id="pm-schema">([\s\S]*?)<\/script>/)[1]);
const theme = {
  _info: {
    source: 'mockup/index.html: блоки theme:begin..theme:end и схема pm-schema',
    units: 'размеры в px (DIP при 100%), длительности в мс, цвета #RRGGBBAA',
    font: ['Segoe UI Variable Text', 'Segoe UI Variable Display', 'Segoe UI'],
    compose: 'итоговая тема = skins[skin] + layouts[layout] + пользовательские settings поверх',
  },
  skins: {},
  layouts: {},
  characters: schema.characters,
  settings: schema.sections.flatMap((sec) => sec.items
    .filter((it) => !['curve', 'action', 'themes'].includes(it.type))
    .map((it) => ({ tab: sec.tab, ...it }))),
  file: { format: schema.format, version: schema.version },
};
function parseBlock(body) {
  const out = {};
  for (const line of body.split(';')) {
    const mm = line.trim().match(/^--([a-z]+)-([\w-]+):\s*(.+)$/s);
    if (!mm) continue;
    const group = GROUPS[mm[1]];
    if (!group) throw new Error('Неизвестный префикс: ' + mm[1]);
    const key = group === 'font' ? mm[1] + '-' + mm[2] : mm[2];
    (out[group] ||= {})[key] = value(group, mm[3].trim().replace(/\s+/g, ' '));
  }
  return out;
}
const re = /:root\[data-(skin|layout)="(\w+)"\]\s*\{([^}]*)\}/g;
let m;
while ((m = re.exec(block))) theme[m[1] + 's'][m[2]] = parseBlock(m[3]);
fs.writeFileSync(path.join(dir, 'theme.json'), JSON.stringify(theme, null, 2) + '\n');
console.log('theme.json: skins', Object.keys(theme.skins).join(', '), '| layouts', Object.keys(theme.layouts).join(', '), '| settings', theme.settings.length);
