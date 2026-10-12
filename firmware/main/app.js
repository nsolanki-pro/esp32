// Shared by all pages: navigation bar, small helpers, the admin password field.
const $ = id => document.getElementById(id);
const sleep = ms => new Promise(r => setTimeout(r, ms));
const getJSON = async url => (await fetch(url, { cache: 'no-store' })).json();

function say(id, text, cls) { $(id).textContent = text; $(id).className = 'msg ' + (cls || ''); }

// Fill a <table> with [label, value] rows (values as text, never HTML).
function rows(table, list) {
  table.innerHTML = '';
  for (const [k, v] of list) {
    if (v === undefined) continue;
    const tr = table.insertRow();
    tr.insertCell().textContent = k;
    tr.insertCell().textContent = v === null ? '–' : v;
  }
}

function duration(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  return d ? `${d} d ${h} h` : h ? `${h} h ${m} min` : m ? `${m} min ${s % 60} s` : `${s} s`;
}

// Navigation bar
(async () => {
  const nav = document.createElement('nav');
  const links = [['/', 'Status'], ['/logs', 'Logs'], ['/update', 'Update'], ['/settings', 'Settings']];
  nav.innerHTML = '<b id="nav-host">ESP32</b>';
  document.body.prepend(nav);
  const add = ([href, text]) => {
    const a = document.createElement('a');
    a.href = href; a.textContent = text;
    if (location.pathname === href) a.className = 'on';
    nav.appendChild(a);
  };
  links.forEach(add);
  try {
    const c = await getJSON('/api/config');
    $('nav-host').textContent = c.hostname;
    if (c.motion) add(['/motion', 'Motion']);
  } catch (e) {}
})();

// Admin password: one field per page (#pw, optional #remember), shared via localStorage.
(() => {
  const pw = $('pw');
  if (!pw) return;
  try { const t = localStorage.getItem('admin_pw'); if (t) { pw.value = t; if ($('remember')) $('remember').checked = true; } } catch (e) {}
})();

// Returns the password, or null (with a message in #msgId) if empty.
function adminPw(msgId) {
  const pw = $('pw').value;
  if (!pw) { if (msgId) say(msgId, 'Enter the admin password at the top of the page.', 'err'); $('pw').focus(); return null; }
  try { $('remember') && $('remember').checked ? localStorage.setItem('admin_pw', pw) : localStorage.removeItem('admin_pw'); } catch (e) {}
  return pw;
}

// POST a form with the admin password. Returns the Response.
function post(url, fields, pw) {
  const headers = { 'Content-Type': 'application/x-www-form-urlencoded' };
  if (pw) headers['X-Admin-Password'] = pw;
  return fetch(url, { method: 'POST', headers, body: new URLSearchParams(fields || {}) });
}

// After a reboot request: wait until the device answers again, then call done(status).
async function waitReboot(msgId, done) {
  const t0 = Date.now();
  let down = false;
  for (;;) {
    await sleep(2000);
    const secs = Math.round((Date.now() - t0) / 1000);
    try {
      const s = await getJSON('/api/status');
      if (down || s.uptime_s < secs) return done(s);
    } catch (e) { down = true; }
    say(msgId, `Rebooting... ${secs} s`, 'warn');
    if (secs > 120) return say(msgId, 'The device did not come back within 2 minutes. Check the setup network or the serial log.', 'err');
  }
}
