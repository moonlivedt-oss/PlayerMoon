// Выгрузка спецификации для C++ из макета mockup/index.html:
//   mockup/themes/<id>.json   - 5 встроенных тем, полный набор значений
//   mockup/layouts.json       - геометрия 4 раскладок, мини-режима и форм обложки
//   mockup/theme-schema.json  - JSON Schema формата playermoon-theme v1
// Источник правды - index.html: блоки theme:begin..theme:end и схема pm-schema.
// Запуск: node tools/export-themes.js
//
// Правила преобразования CSS в значения для C++:
//   цвет                    -> "#RRGGBBAA"
//   var(--c-accent)         -> { "ref": "accent" }
//   color-mix(X 10%, transp)-> { "ref": "accent", "alpha": 0.1 }
//   тень                    -> [{ x, y, blur, spread, color, inset }]
//   cubic-bezier(a,b,c,d)   -> [a, b, c, d]
//   12px / 220ms            -> 12 / 220
// Выборка пружины linear(...) и --dur-spring не выгружаются: в C++ они
// считаются из spring.stiffness и spring.damping (см. ANIMATIONS.md).
const fs = require('fs');
const path = require('path');

const dir = path.join(__dirname, '..');
const html = fs.readFileSync(path.join(dir, 'index.html'), 'utf8');
const block = html.slice(html.indexOf('/* theme:begin */'), html.indexOf('/* theme:end */'));
const schema = JSON.parse(html.match(/<script type="application\/json" id="pm-schema">([\s\S]*?)<\/script>/)[1]);

const GROUPS = {
  c: 'color', r: 'radius', sh: 'shadow', sp: 'spacing', fs: 'font', fw: 'font', ls: 'font', tt: 'font',
  sz: 'size', dur: 'duration', ease: 'easing', spring: 'spring', fx: 'effect', viz: 'visualizer', mq: 'marquee',
  opt: 'defaults',
};
// Имена переменных-цветов, на которые можно сослаться из производного цвета
const REFS = { '--c-accent': 'accent', '--c-accent-2': 'accent2', '--c-text': 'text', '--c-glow': 'glow', '--c-win-solid': 'windowSolid' };

const hex2 = (n) => Math.round(n).toString(16).padStart(2, '0').toUpperCase();
function plainColor(v) {
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
// Цвет, в том числе производный от акцента
function color(v) {
  v = v.trim();
  const p = plainColor(v);
  if (p) return p;
  let m = v.match(/^var\((--[\w-]+)\)$/);
  if (m && REFS[m[1]]) return { ref: REFS[m[1]] };
  m = v.match(/^color-mix\(in srgb, var\((--[\w-]+)\) ([\d.]+)%, transparent\)$/);
  if (m && REFS[m[1]]) return { ref: REFS[m[1]], alpha: +(parseFloat(m[2]) / 100).toFixed(3) };
  throw new Error('Не удалось разобрать цвет: ' + v);
}
// Делит строку по запятым верхнего уровня (внутри скобок не делит)
function splitTop(v) {
  const parts = [];
  let depth = 0, cur = '';
  for (const ch of v) {
    if (ch === '(') depth++;
    if (ch === ')') depth--;
    if (ch === ',' && depth === 0) { parts.push(cur.trim()); cur = ''; } else cur += ch;
  }
  parts.push(cur.trim());
  return parts;
}
function shadows(v) {
  return splitTop(v).map((p) => {
    const inset = /\binset\b/.test(p);
    p = p.replace(/\binset\b/, '').trim();
    // Цвет - последний токен: rgba(...), color-mix(...), var(...) или #hex
    const start = p.search(/(rgba?\(|color-mix\(|var\(|#[0-9a-f]{3,8})/i);
    const col = p.slice(start).trim();
    const nums = p.slice(0, start).trim().split(/\s+/).map(parseFloat);
    const [x = 0, y = 0, blur = 0, spread = 0] = nums;
    return { x, y, blur, spread, color: color(col), inset };
  });
}
function value(group, raw) {
  const v = raw.replace(/\s+/g, ' ').trim();
  if (group === 'color') return color(v);
  if (group === 'shadow') return shadows(v);
  if (group === 'easing') {
    const m = v.match(/^cubic-bezier\(([^)]+)\)$/);
    if (m) return m[1].split(',').map(Number);
    return undefined;   // linear(...) - не выгружается
  }
  if (/^-?[\d.]+(px|ms)$/.test(v) || /^-?[\d.]+$/.test(v)) return parseFloat(v);
  return v;
}
const camel = (s) => s.replace(/-(\w)/g, (_, c) => c.toUpperCase());

// Разбор блоков :root[data-skin|layout="id"] { ... } в сырые строки
function rawBlocks(kind) {
  const out = {};
  const re = new RegExp(`:root\\[data-${kind}="(\\w+)"\\]\\s*\\{([^}]*)\\}`, 'g');
  let m;
  while ((m = re.exec(block))) {
    const vars = {};
    for (const line of m[2].split(';')) {
      const mm = line.trim().match(/^(--[\w-]+):\s*(.+)$/s);
      if (mm) vars[mm[1]] = mm[2].trim();
    }
    out[m[1]] = vars;
  }
  return out;
}
function structured(vars, skip) {
  const out = {};
  for (const [name, raw] of Object.entries(vars)) {
    if (skip(name)) continue;
    const mm = name.match(/^--([a-z]+)-([\w-]+)$/);
    const group = GROUPS[mm[1]];
    if (!group) throw new Error('Неизвестный префикс: ' + name);
    const key = camel(group === 'font' ? mm[1] + '-' + mm[2] : mm[2]);
    const val = value(group, raw);
    if (val === undefined) continue;
    (out[group] ||= {})[key] = val;
  }
  return out;
}

const ITEMS = schema.sections.flatMap((s) => s.items.map((it) => ({ ...it, tab: s.tab })));
const VALUE_TYPES = ['seg', 'range', 'color', 'toggle', 'skins'];
const THEME_ITEMS = ITEMS.filter((it) => it.theme !== false && VALUE_TYPES.includes(it.type));

// Значения настроек по умолчанию для пресета: тот же алгоритм, что readSkin() в макете
function settingsDefaults(id, vars) {
  const d = {};
  for (const it of THEME_ITEMS) {
    if (it.type === 'skins') { d.skin = id; continue; }
    if (it.default !== undefined) { d[it.key] = it.default; continue; }
    const raw = vars[it.src];
    if (raw === undefined) throw new Error(`${id}: нет ${it.src}`);
    if (it.type === 'color') d[it.key] = plainColor(raw).slice(0, 7).toLowerCase();
    else if (it.type === 'toggle') d[it.key] = raw.trim() === '1';
    else if (it.type === 'range') {
      if (it.key === 'winDensity') {
        const c = plainColor(raw);
        d[it.key] = +(parseInt(c.slice(7, 9), 16) / 255).toFixed(2);
      } else d[it.key] = parseFloat(raw);
    } else d[it.key] = raw.trim();
  }
  return d;
}

// ---------- Встроенные темы ----------
// Переменные, которые являются настройками, лежат только в settings,
// чтобы одно значение не хранилось в двух местах
const SETTING_SRC = new Set(THEME_ITEMS.map((it) => it.src).filter(Boolean));
const skins = rawBlocks('skin');
const names = Object.fromEntries(schema.skins);
fs.mkdirSync(path.join(dir, 'themes'), { recursive: true });
for (const [id, vars] of Object.entries(skins)) {
  const theme = {
    format: schema.format,
    version: schema.version,
    kind: 'builtin',
    id,
    name: names[id],
    settings: settingsDefaults(id, vars),
    material: structured(vars, (n) => n.startsWith('--opt-') || n === '--dur-spring' || SETTING_SRC.has(n)),
  };
  fs.writeFileSync(path.join(dir, 'themes', id + '.json'), JSON.stringify(theme, null, 2) + '\n');
}

// ---------- Раскладки ----------
const layouts = rawBlocks('layout');
const layoutsOut = {
  _info: 'Геометрия раскладок в px (DIP при 100%). Описание и различия - LAYOUTS.md.',
  layouts: Object.fromEntries(Object.entries(layouts).map(([id, vars]) => [id, structured(vars, () => false)])),
  mini: { width: 360, height: 88, cornerRadius: 'min(radius.win, 16)' },
  coverShape: {
    square: { cover: 2, miniCover: 2 },
    rounded: { cover: 18, miniCover: 10 },
    circle: { cover: 'circle', miniCover: 'circle' },
    vinyl: { cover: 'circle', miniCover: 'circle', grooves: { step: 5, line: 1, color: '#00000021' }, hole: { diameter: 22, ring: 2 } },
  },
};
fs.writeFileSync(path.join(dir, 'layouts.json'), JSON.stringify(layoutsOut, null, 2) + '\n');

// ---------- JSON Schema ----------
function settingSchema(it) {
  if (it.type === 'skins') return { enum: schema.skins.map(([id]) => id) };
  if (it.type === 'seg') return { enum: it.options.map(([v]) => v) };
  if (it.type === 'range') return { type: 'number', minimum: it.min, maximum: it.max };
  if (it.type === 'color') return { $ref: '#/$defs/settingColor' };
  if (it.type === 'toggle') return { type: 'boolean' };
  throw new Error(it.type);
}
const settingProps = Object.fromEntries(THEME_ITEMS.map((it) => [it.key, { description: it.label, ...settingSchema(it) }]));
// Материал: набор групп и ключей берётся из темы «Стекло», у всех тем он одинаковый
const glass = JSON.parse(fs.readFileSync(path.join(dir, 'themes', 'glass.json'), 'utf8')).material;
const leafSchema = (group, v) => {
  if (group === 'color') return { $ref: '#/$defs/colorValue' };
  if (group === 'shadow') return { $ref: '#/$defs/shadowList' };
  if (group === 'easing') return { $ref: '#/$defs/cubicBezier' };
  return typeof v === 'number' ? { type: 'number' } : { type: 'string' };
};
const materialProps = {};
for (const [group, obj] of Object.entries(glass)) {
  materialProps[group] = {
    type: 'object',
    additionalProperties: false,
    required: Object.keys(obj),
    properties: Object.fromEntries(Object.entries(obj).map(([k, v]) => [k, leafSchema(group, v)])),
  };
}
const themeSchema = {
  $schema: 'https://json-schema.org/draft/2020-12/schema',
  $id: 'playermoon-theme-v1',
  title: 'Файл темы PlayerMoon, формат playermoon-theme, версия 1',
  description: 'Встроенная тема (kind = builtin) содержит полный набор значений: settings со всеми ключами и material. Пользовательская тема (kind = user или без kind) содержит settings: settings.skin - базовая встроенная тема, остальные ключи - переопределения поверх неё. Приложение игнорирует неизвестные ключи и обрезает числа по диапазону, схема строже, чтобы ловить ошибки в собственных файлах.',
  oneOf: [{ $ref: '#/$defs/builtinTheme' }, { $ref: '#/$defs/userTheme' }],
  $defs: {
    hex8: { type: 'string', pattern: '^#[0-9A-F]{8}$', description: 'Цвет #RRGGBBAA' },
    settingColor: { type: 'string', pattern: '^#[0-9a-fA-F]{6}$', description: 'Цвет настройки #RRGGBB' },
    colorRef: {
      type: 'object',
      additionalProperties: false,
      required: ['ref'],
      properties: {
        ref: { enum: ['accent', 'accent2', 'text', 'glow', 'windowSolid'], description: 'glow = accent с прозрачностью effect.glow; windowSolid = цвет фона окна без прозрачности' },
        alpha: { type: 'number', minimum: 0, maximum: 1, description: 'Множитель прозрачности (color-mix с transparent)' },
      },
    },
    colorValue: { oneOf: [{ $ref: '#/$defs/hex8' }, { $ref: '#/$defs/colorRef' }] },
    shadowList: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['x', 'y', 'blur', 'spread', 'color', 'inset'],
        properties: {
          x: { type: 'number' }, y: { type: 'number' }, blur: { type: 'number', minimum: 0 }, spread: { type: 'number' },
          color: { $ref: '#/$defs/colorValue' }, inset: { type: 'boolean' },
        },
      },
    },
    cubicBezier: { type: 'array', items: { type: 'number' }, minItems: 4, maxItems: 4 },
    settings: { type: 'object', additionalProperties: false, properties: settingProps },
    material: { type: 'object', additionalProperties: false, required: Object.keys(materialProps), properties: materialProps },
    builtinTheme: {
      type: 'object',
      additionalProperties: false,
      required: ['format', 'version', 'kind', 'id', 'name', 'settings', 'material'],
      properties: {
        format: { const: schema.format },
        version: { const: schema.version },
        kind: { const: 'builtin' },
        id: { enum: schema.skins.map(([id]) => id) },
        name: { type: 'string' },
        settings: { allOf: [{ $ref: '#/$defs/settings' }, { required: THEME_ITEMS.map((it) => it.key) }] },
        material: { $ref: '#/$defs/material' },
      },
    },
    userTheme: {
      type: 'object',
      additionalProperties: false,
      required: ['format', 'version', 'name', 'settings'],
      properties: {
        format: { const: schema.format },
        version: { const: schema.version },
        kind: { const: 'user' },
        name: { type: 'string', maxLength: 40 },
        settings: { allOf: [{ $ref: '#/$defs/settings' }, { required: ['skin'] }] },
      },
    },
  },
};
fs.writeFileSync(path.join(dir, 'theme-schema.json'), JSON.stringify(themeSchema, null, 2) + '\n');
console.log('themes:', Object.keys(skins).join(', '), '| layouts:', Object.keys(layouts).join(', '), '| settings in theme:', THEME_ITEMS.length);
