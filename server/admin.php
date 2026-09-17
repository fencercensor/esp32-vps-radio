<?php
declare(strict_types=1);

require_once __DIR__ . '/config-store.php';

$isHttps = (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off');
session_name('esp_radio_admin');
session_set_cookie_params([
    'lifetime' => 0,
    'path' => '/esp-radio/',
    'secure' => $isHttps,
    'httponly' => true,
    'samesite' => 'Strict',
]);
session_start();
header('Cache-Control: no-store');
header('X-Content-Type-Options: nosniff');
header('X-Frame-Options: DENY');
header("Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'");

function h(string $value): string
{
    return htmlspecialchars($value, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
}

function redirectAdmin(): never
{
    header('Location: admin.php');
    exit;
}

function ensureDataDirectory(): void
{
    if (!is_dir(RADIO_DATA_DIR) && !@mkdir(RADIO_DATA_DIR, 0750, true) && !is_dir(RADIO_DATA_DIR)) {
        throw new RuntimeException('تعذر إنشاء مجلد البيانات.');
    }
}

function csrfToken(): string
{
    if (empty($_SESSION['csrf'])) {
        $_SESSION['csrf'] = bin2hex(random_bytes(24));
    }
    return (string) $_SESSION['csrf'];
}

function validCsrf(): bool
{
    return isset($_POST['csrf']) && hash_equals(csrfToken(), (string) $_POST['csrf']);
}

$adminExists = is_file(RADIO_ADMIN_FILE);
$loggedIn = !empty($_SESSION['authenticated']);
$error = '';
$success = isset($_GET['saved']) ? 'تم حفظ التعديلات. ستصل إلى أجهزة ESP تلقائيًا.' : '';

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    try {
        if (!validCsrf()) {
            throw new RuntimeException('انتهت الجلسة. أعد المحاولة.');
        }
        $action = (string) ($_POST['action'] ?? '');
        if ($action === 'setup' && !$adminExists) {
            $password = (string) ($_POST['password'] ?? '');
            $confirmation = (string) ($_POST['password_confirmation'] ?? '');
            if (strlen($password) < 10) {
                throw new RuntimeException('اختر كلمة مرور لا تقل عن 10 أحرف.');
            }
            if (!hash_equals($password, $confirmation)) {
                throw new RuntimeException('تأكيد كلمة المرور غير مطابق.');
            }
            ensureDataDirectory();
            $record = json_encode(['password_hash' => password_hash($password, PASSWORD_DEFAULT)]);
            if (!is_string($record) || @file_put_contents(RADIO_ADMIN_FILE, $record, LOCK_EX) === false) {
                throw new RuntimeException('تعذر إنشاء حساب الإدارة.');
            }
            @chmod(RADIO_ADMIN_FILE, 0640);
            $_SESSION['authenticated'] = true;
            session_regenerate_id(true);
            redirectAdmin();
        }
        if ($action === 'login' && $adminExists) {
            $raw = @file_get_contents(RADIO_ADMIN_FILE);
            $record = is_string($raw) ? json_decode($raw, true) : null;
            $hash = is_array($record) ? (string) ($record['password_hash'] ?? '') : '';
            $attempts = (int) ($_SESSION['login_attempts'] ?? 0);
            $lastAttempt = (int) ($_SESSION['last_attempt'] ?? 0);
            if ($attempts >= 5 && time() - $lastAttempt < 60) {
                throw new RuntimeException('محاولات كثيرة. انتظر دقيقة ثم حاول مرة أخرى.');
            }
            if ($hash === '' || !password_verify((string) ($_POST['password'] ?? ''), $hash)) {
                $_SESSION['login_attempts'] = $attempts + 1;
                $_SESSION['last_attempt'] = time();
                throw new RuntimeException('كلمة المرور غير صحيحة.');
            }
            $_SESSION['login_attempts'] = 0;
            $_SESSION['authenticated'] = true;
            session_regenerate_id(true);
            redirectAdmin();
        }
        if ($action === 'logout') {
            $_SESSION = [];
            session_destroy();
            redirectAdmin();
        }
        if ($action === 'save' && $loggedIn) {
            $payload = json_decode((string) ($_POST['payload'] ?? ''), true);
            if (!is_array($payload)) {
                throw new RuntimeException('بيانات الحفظ غير صحيحة.');
            }
            saveRadioConfig($payload);
            header('Location: admin.php?saved=1');
            exit;
        }
        throw new RuntimeException('الطلب غير مسموح.');
    } catch (Throwable $exception) {
        $error = $exception->getMessage();
    }
    $adminExists = is_file(RADIO_ADMIN_FILE);
    $loggedIn = !empty($_SESSION['authenticated']);
}

$config = loadRadioConfig();
$configJson = json_encode($config, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_HEX_TAG | JSON_HEX_AMP | JSON_HEX_APOS | JSON_HEX_QUOT);
?><!doctype html>
<html lang="ar" dir="rtl">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>إدارة راديو ESP</title>
  <link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'%3E%3Crect width='64' height='64' rx='14' fill='%230b1220'/%3E%3Cpath d='M18 27h28v22H18zM22 15l23-7M25 38a7 7 0 1014 0 7 7 0 00-14 0z' fill='none' stroke='%2338bdf8' stroke-width='4' stroke-linecap='round'/%3E%3Ccircle cx='43' cy='33' r='2' fill='%2322c55e'/%3E%3C/svg%3E">
  <style>
    :root{color-scheme:dark;--bg:#070b12;--panel:#0d1523;--panel2:#111c2d;--line:#26354a;--text:#edf6ff;--muted:#9db0c6;--cyan:#38bdf8;--green:#22c55e;--red:#fb7185;--shadow:0 18px 50px #0007}
    *{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 85% 5%,#0e3247 0,transparent 32rem),var(--bg);color:var(--text);font-family:Tahoma,"Segoe UI",sans-serif;font-size:16px;line-height:1.55;min-height:100vh}button,input,textarea{font:inherit}button{cursor:pointer}.shell{width:min(1120px,calc(100% - 28px));margin:auto;padding:28px 0 60px}.top{display:flex;align-items:center;justify-content:space-between;gap:16px;margin-bottom:22px}.brand{display:flex;align-items:center;gap:13px}.mark{display:grid;place-items:center;width:48px;height:48px;border:1px solid #2d5269;background:#0b1d2b;border-radius:15px;color:var(--cyan);font-size:24px;box-shadow:var(--shadow)}h1{font-size:clamp(1.35rem,4vw,2rem);margin:0}.sub{color:var(--muted);font-size:.9rem}.panel,.station{background:linear-gradient(145deg,var(--panel),#0a111d);border:1px solid var(--line);border-radius:18px;box-shadow:var(--shadow)}.panel{padding:22px;margin-bottom:18px}.login{width:min(480px,100%);margin:10vh auto 0}.login h1{margin-bottom:8px}.field{display:grid;gap:7px;margin:14px 0}.field span,.label{color:#bfd1e3;font-size:.9rem;font-weight:700}input,textarea{width:100%;border:1px solid #30445d;border-radius:11px;background:#070d17;color:var(--text);padding:11px 12px;outline:none}input:focus,textarea:focus{border-color:var(--cyan);box-shadow:0 0 0 3px #38bdf822}textarea{min-height:116px;resize:vertical;direction:ltr;text-align:left}.btn{border:0;border-radius:11px;padding:10px 15px;color:#04131d;background:var(--cyan);font-weight:800}.btn.secondary{background:#17273a;color:#dcecff;border:1px solid #30445d}.btn.danger{background:#341722;color:#fecdd3;border:1px solid #713047}.btn.small{padding:7px 10px;font-size:.86rem}.notice{padding:12px 14px;border-radius:12px;margin:12px 0}.notice.error{background:#3c1620;border:1px solid #7f273a;color:#fecdd3}.notice.success{background:#0c3527;border:1px solid #176b4d;color:#bbf7d0}.grid{display:grid;grid-template-columns:1fr 1fr;gap:18px}.status{display:flex;gap:10px;align-items:center;color:#c8d7e6}.dot{width:9px;height:9px;border-radius:50%;background:var(--green);box-shadow:0 0 14px var(--green)}.toolbar{display:flex;align-items:center;justify-content:space-between;gap:12px;margin:18px 0 12px}.toolbar h2{margin:0;font-size:1.15rem}.stations{display:grid;gap:13px}.station{padding:16px}.station-head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:11px}.station-title{font-weight:800}.actions{display:flex;gap:7px;flex-wrap:wrap}.station-grid{display:grid;grid-template-columns:minmax(150px,.65fr) 1.35fr;gap:12px}.check{display:flex;align-items:center;gap:9px;color:#c8d7e6}.check input{width:18px;height:18px}.footer-actions{position:sticky;bottom:12px;display:flex;justify-content:flex-start;gap:10px;margin-top:18px;padding:12px;background:#08111cdd;backdrop-filter:blur(10px);border:1px solid var(--line);border-radius:15px}.hint{color:var(--muted);font-size:.85rem;margin-top:6px}.empty{padding:28px;text-align:center;color:var(--muted);border:1px dashed var(--line);border-radius:15px}@media(max-width:720px){.grid,.station-grid{grid-template-columns:1fr}.top{align-items:flex-start}.shell{width:min(100% - 18px,1120px);padding-top:16px}.panel{padding:16px}.station-head{align-items:flex-start;flex-direction:column}.footer-actions{justify-content:stretch}.footer-actions .btn{flex:1}}
  </style>
</head>
<body>
<?php if (!$adminExists || !$loggedIn): ?>
  <main class="shell"><section class="panel login">
    <div class="brand"><div class="mark">◉</div><div><h1>إدارة راديو ESP</h1><div class="sub">إعداد مركزي لكل الأجهزة</div></div></div>
    <?php if ($error !== ''): ?><div class="notice error"><?= h($error) ?></div><?php endif; ?>
    <?php if (!$adminExists): ?>
      <p>أنشئ كلمة مرور الإدارة لأول مرة. لن تُحفظ إلا كقيمة مشفّرة.</p>
      <form method="post">
        <input type="hidden" name="csrf" value="<?= h(csrfToken()) ?>"><input type="hidden" name="action" value="setup">
        <label class="field"><span>كلمة المرور الجديدة</span><input name="password" type="password" minlength="10" autocomplete="new-password" required></label>
        <label class="field"><span>تأكيد كلمة المرور</span><input name="password_confirmation" type="password" minlength="10" autocomplete="new-password" required></label>
        <button class="btn" type="submit">إنشاء لوحة التحكم</button>
      </form>
    <?php else: ?>
      <form method="post">
        <input type="hidden" name="csrf" value="<?= h(csrfToken()) ?>"><input type="hidden" name="action" value="login">
        <label class="field"><span>كلمة المرور</span><input name="password" type="password" autocomplete="current-password" required autofocus></label>
        <button class="btn" type="submit">دخول</button>
      </form>
    <?php endif; ?>
  </section></main>
<?php else: ?>
  <main class="shell">
    <header class="top"><div class="brand"><div class="mark">◉</div><div><h1>إدارة راديو ESP</h1><div class="sub">الإصدار <?= (int) $config['version'] ?></div></div></div><form method="post"><input type="hidden" name="csrf" value="<?= h(csrfToken()) ?>"><input type="hidden" name="action" value="logout"><button class="btn secondary small">خروج</button></form></header>
    <?php if ($error !== ''): ?><div class="notice error"><?= h($error) ?></div><?php endif; ?>
    <?php if ($success !== ''): ?><div class="notice success"><?= h($success) ?></div><?php endif; ?>
    <form method="post" id="config-form">
      <input type="hidden" name="csrf" value="<?= h(csrfToken()) ?>"><input type="hidden" name="action" value="save"><input type="hidden" name="payload" id="payload">
      <section class="panel">
        <div><div class="status"><i class="dot"></i><strong>الإعداد المركزي يعمل</strong></div><div class="hint">كل أجهزة ESP تسحب نفس قائمة المحطات تلقائيًا.</div></div>
      </section>
      <div class="toolbar"><h2>المحطات وروابط التشغيل</h2><button class="btn secondary" type="button" id="add-station">+ إضافة محطة</button></div>
      <section class="stations" id="stations"></section>
      <div class="footer-actions"><button class="btn" type="submit">حفظ وإرسال لكل الأجهزة</button><button class="btn secondary" type="button" id="reset-view">تراجع عن التعديلات</button></div>
    </form>
  </main>
  <script>
  const original = <?= $configJson ?: '{}' ?>;
  let state = JSON.parse(JSON.stringify(original));
  const stationsNode = document.getElementById('stations');
  const esc = value => String(value).replace(/[&<>'"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[c]));
  function render(){
    stationsNode.innerHTML=state.stations.length?'':'<div class="empty">أضف محطة واحدة على الأقل.</div>';
    state.stations.forEach((s,i)=>{
      const card=document.createElement('article');card.className='station';
      card.innerHTML=`<div class="station-head"><div class="station-title">${esc(s.name||'محطة جديدة')}</div><div class="actions"><button type="button" class="btn secondary small" data-act="up">↑</button><button type="button" class="btn secondary small" data-act="down">↓</button><button type="button" class="btn danger small" data-act="delete">حذف</button></div></div><div class="station-grid"><div><label class="field"><span>اسم المحطة</span><input data-key="name" maxlength="60" value="${esc(s.name)}" required></label><label class="field"><span>المعرّف الإنجليزي</span><input data-key="id" maxlength="32" pattern="[a-z0-9][a-z0-9_-]{0,31}" value="${esc(s.id)}" required></label><label class="check"><input data-key="enabled" type="checkbox" ${s.enabled?'checked':''}> المحطة مفعّلة</label></div><label class="field" style="margin:0"><span>الروابط بالترتيب — رابط في كل سطر</span><textarea data-key="sources" required>${esc((s.sources||[]).join('\n'))}</textarea><span class="hint">إذا تعطل الرابط الأول يجرب السيرفر التالي تلقائيًا.</span></label></div>`;
      card.querySelectorAll('[data-key]').forEach(el=>el.addEventListener('input',()=>{const k=el.dataset.key;s[k]=k==='enabled'?el.checked:k==='sources'?el.value.split(/\r?\n/).map(x=>x.trim()).filter(Boolean):el.value;if(k==='name')card.querySelector('.station-title').textContent=el.value||'محطة جديدة'}));
      card.querySelector('[data-act=delete]').onclick=()=>{if(confirm('حذف هذه المحطة؟')){state.stations.splice(i,1);render()}};
      card.querySelector('[data-act=up]').onclick=()=>{if(i>0){[state.stations[i-1],state.stations[i]]=[state.stations[i],state.stations[i-1]];render()}};
      card.querySelector('[data-act=down]').onclick=()=>{if(i<state.stations.length-1){[state.stations[i+1],state.stations[i]]=[state.stations[i],state.stations[i+1]];render()}};
      stationsNode.appendChild(card);
    });
  }
  document.getElementById('add-station').onclick=()=>{state.stations.push({id:'station_'+(state.stations.length+1),name:'محطة جديدة',enabled:true,sources:['https://']});render();window.scrollTo({top:document.body.scrollHeight,behavior:'smooth'})};
  document.getElementById('reset-view').onclick=()=>{state=JSON.parse(JSON.stringify(original));render()};
  document.getElementById('config-form').addEventListener('submit',e=>{if(!state.stations.length){e.preventDefault();alert('أضف محطة واحدة على الأقل.');return}document.getElementById('payload').value=JSON.stringify(state)});
  render();
  </script>
<?php endif; ?>
</body>
</html>
