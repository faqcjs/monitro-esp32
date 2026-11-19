"use strict";

const ONLINE_WINDOW_MS = 65 * 1000; // device heartbeat is every 20 s: offline after 3 missed beats
const REFRESH_MS = 15 * 1000; // network poll
const TICK_MS = 5 * 1000; // local re-render so "online" and "ago" texts expire on time
const CAUTION_RATIO = 0.75; // meter turns amber at 75 % of the threshold

const sb = window.supabase.createClient(
  window.APP_CONFIG.supabaseUrl,
  window.APP_CONFIG.supabaseAnonKey
);

const $ = (id) => document.getElementById(id);
const VIEWS = ["view-login", "view-register", "view-dashboard"];

const state = {
  userId: null,
  device: null,
  settings: null, // last saved settings row
  lastMotion: null,
  hourly: [],
  chart: null,
  timer: null,
  tick: null,
  todayCount: null,
  authMode: "login",
};

// ================================================================ helpers

function show(viewId) {
  for (const id of VIEWS) $(id).hidden = id !== viewId;
  const inApp = viewId !== "view-login";
  $("logout").hidden = !inApp;
  $("conn").hidden = viewId !== "view-dashboard";
  if (viewId !== "view-dashboard") $("brand-device").textContent = "";
}

function notice(id, text, kind = "") {
  const el = $(id);
  el.textContent = text;
  el.hidden = !text;
  el.classList.toggle("is-error", kind === "error");
  el.classList.toggle("is-ok", kind === "ok");
}

function css(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

function fmtDuration(minutes) {
  const total = Math.max(0, Math.round(minutes));
  const d = Math.floor(total / 1440);
  const h = Math.floor((total % 1440) / 60);
  const m = total % 60;
  if (d > 0) return h ? `${d} d ${h} h` : `${d} d`;
  if (h > 0) return m ? `${h} h ${m} min` : `${h} h`;
  return `${m} min`;
}

function minutesSince(date) {
  return (Date.now() - date.getTime()) / 60000;
}

function fmtClock(date) {
  return date.toLocaleTimeString("es-AR", { hour: "2-digit", minute: "2-digit" });
}

function fmtWhen(date) {
  const today = new Date();
  const sameDay = date.toDateString() === today.toDateString();
  const yesterday = new Date(today.getTime() - 86400000).toDateString() === date.toDateString();
  if (sameDay) return `hoy a las ${fmtClock(date)}`;
  if (yesterday) return `ayer a las ${fmtClock(date)}`;
  return `${date.toLocaleDateString("es-AR", { day: "2-digit", month: "2-digit" })} a las ${fmtClock(date)}`;
}

function fmtHour(h) {
  return `${String(h).padStart(2, "0")}:00`;
}

function inWindow(hour, s) {
  if (!s || !s.use_day_window) return true;
  if (s.day_start_h <= s.day_end_h) return hour >= s.day_start_h && hour < s.day_end_h;
  return hour >= s.day_start_h || hour < s.day_end_h;
}

function signalQuality(rssi) {
  if (rssi == null) return "—";
  if (rssi >= -60) return "Excelente";
  if (rssi >= -70) return "Buena";
  if (rssi >= -80) return "Regular";
  return "Débil";
}

function startOfToday() {
  const d = new Date();
  d.setHours(0, 0, 0, 0);
  return d;
}

// ================================================================ auth

function setAuthMode(mode) {
  state.authMode = mode;
  const signup = mode === "signup";
  $("tab-login").classList.toggle("is-active", !signup);
  $("tab-signup").classList.toggle("is-active", signup);
  $("tab-login").setAttribute("aria-selected", String(!signup));
  $("tab-signup").setAttribute("aria-selected", String(signup));
  $("login-submit").textContent = signup ? "Crear cuenta" : "Ingresar";
  $("login-password").autocomplete = signup ? "new-password" : "current-password";
  $("password-hint").hidden = !signup;
  notice("login-msg", "");
}

$("tab-login").addEventListener("click", () => setAuthMode("login"));
$("tab-signup").addEventListener("click", () => setAuthMode("signup"));

$("login-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const email = $("login-email").value.trim();
  const password = $("login-password").value;
  if (!email || !$("login-email").checkValidity()) return notice("login-msg", "Ingrese un email válido.", "error");
  if (password.length < 6) return notice("login-msg", "La contraseña debe tener al menos 6 caracteres.", "error");

  const btn = $("login-submit");
  btn.disabled = true;
  try {
    if (state.authMode === "signup") {
      notice("login-msg", "Creando la cuenta…");
      const { data, error } = await sb.auth.signUp({ email, password });
      if (error) return notice("login-msg", `No se pudo crear la cuenta: ${error.message}`, "error");
      if (!data.session) {
        setAuthMode("login");
        notice("login-msg", "Cuenta creada. Abra el enlace que le enviamos por email y después ingrese.", "ok");
      }
    } else {
      notice("login-msg", "Ingresando…");
      const { error } = await sb.auth.signInWithPassword({ email, password });
      if (error) notice("login-msg", "Email o contraseña incorrectos. Revise los datos e intente de nuevo.", "error");
    }
  } finally {
    btn.disabled = false;
  }
});

$("logout").addEventListener("click", () => sb.auth.signOut());

sb.auth.onAuthStateChange((event, session) => {
  // Token refreshes (and repeated SIGNED_IN on tab focus) must not reset the screen.
  const userId = session ? session.user.id : null;
  if (event !== "SIGNED_OUT" && userId !== null && userId === state.userId) return;
  state.userId = userId;
  // Defer so Supabase calls do not run inside the auth callback.
  setTimeout(() => route(session), 0);
});

async function route(session) {
  clearInterval(state.timer);
  clearInterval(state.tick);
  state.device = null;
  if (!session) {
    state.userId = null;
    notice("login-msg", "");
    return show("view-login");
  }

  const { data, error } = await sb
    .from("devices")
    .select("*")
    .order("created_at", { ascending: true })
    .limit(1);
  if (error) {
    show("view-login");
    return notice("login-msg", `No se pudieron leer los aparatos: ${error.message}`, "error");
  }
  if (data.length === 0) return showRegister();

  state.device = data[0];
  show("view-dashboard");
  $("brand-device").textContent = state.device.name;
  await loadSettings();
  await refresh();
  state.timer = setInterval(refresh, REFRESH_MS);
  state.tick = setInterval(() => {
    if (!state.device) return;
    renderStatus();
    renderMetrics(state.todayCount);
  }, TICK_MS);
}

// ================================================================ registration

function setStep(id, { enabled, done }) {
  const el = $(id);
  el.setAttribute("aria-disabled", String(!enabled));
  el.classList.toggle("is-done", Boolean(done));
}

function showRegister() {
  $("register-form").hidden = false;
  $("token-box").hidden = true;
  $("token").textContent = "";
  $("token-done").disabled = true;
  setStep("step-name", { enabled: true });
  setStep("step-token", { enabled: false });
  setStep("step-flash", { enabled: false });
  notice("register-msg", "");
  show("view-register");
}

$("register-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const name = $("device-name").value.trim();
  if (!name) return notice("register-msg", "Escriba un nombre para el aparato.", "error");
  $("register-submit").disabled = true;
  notice("register-msg", "Registrando…");
  const { data, error } = await sb.rpc("register_device", { p_name: name });
  $("register-submit").disabled = false;
  if (error) return notice("register-msg", `No se pudo registrar: ${error.message}`, "error");

  notice("register-msg", "");
  $("token").textContent = data;
  $("token-box").hidden = false;
  $("register-form").hidden = true;
  setStep("step-name", { enabled: true, done: true });
  setStep("step-token", { enabled: true });
  setStep("step-flash", { enabled: true });
  $("token-done").disabled = false;
});

$("token-copy").addEventListener("click", async () => {
  const text = $("token").textContent;
  try {
    await navigator.clipboard.writeText(text);
    $("token-copy").textContent = "Copiado";
    setStep("step-token", { enabled: true, done: true });
  } catch {
    const range = document.createRange();
    range.selectNodeContents($("token"));
    const sel = window.getSelection();
    sel.removeAllRanges();
    sel.addRange(range);
    $("token-copy").textContent = "Seleccionado: use Ctrl+C";
  }
});

$("token-done").addEventListener("click", async () => {
  $("token").textContent = "";
  const { data } = await sb.auth.getSession();
  route(data.session);
});

// ================================================================ dashboard

async function refresh() {
  if (!state.device) return;
  const id = state.device.id;
  const since24h = new Date(Date.now() - 24 * 3600 * 1000);

  const [dev, last, today, hourly] = await Promise.all([
    sb.from("devices").select("*").eq("id", id).single(),
    sb.from("events").select("created_at").eq("device_id", id).eq("kind", "motion")
      .order("created_at", { ascending: false }).limit(1),
    sb.from("events").select("id", { count: "exact", head: true }).eq("device_id", id)
      .eq("kind", "motion").gte("created_at", startOfToday().toISOString()),
    sb.rpc("motion_by_hour", { p_device: id, p_since: since24h.toISOString() }),
  ]);

  if (!dev.error) state.device = dev.data;
  if (!last.error) state.lastMotion = last.data[0] ? new Date(last.data[0].created_at) : null;
  if (!hourly.error) state.hourly = hourly.data;

  renderStatus();
  if (!today.error) state.todayCount = today.count;
  renderMetrics(state.todayCount);
  renderChart();
  $("updated").textContent = `Actualizado a las ${new Date().toLocaleTimeString("es-AR")}`;
}

function renderStatus() {
  const d = state.device;
  const s = state.settings;
  const lastSeen = d.last_seen ? new Date(d.last_seen) : null;
  const online = Boolean(lastSeen) && Date.now() - lastSeen.getTime() < ONLINE_WINDOW_MS;
  const paused = s && !inWindow(new Date().getHours(), s);

  // Connection indicator in the app bar
  $("conn").classList.toggle("is-online", online);
  $("conn-text").textContent = online ? "En línea" : "Sin conexión";

  // Meter: time without motion against the alert threshold
  const threshold = s ? s.inactivity_min : null;
  const silent = state.lastMotion ? minutesSince(state.lastMotion) : null;
  const ratio = threshold && silent != null ? Math.min(1, silent / threshold) : 0;
  $("meter-fill").style.width = `${Math.round(ratio * 100)}%`;
  $("meter-track").setAttribute("aria-valuenow", String(Math.round(ratio * 100)));
  $("meter-value").textContent = silent != null ? fmtDuration(silent) : "Sin registros";
  $("meter-max").textContent = threshold ? `Alerta a las ${fmtDuration(threshold)}` : "—";

  let level, pill, title, detail;
  const lastText = state.lastMotion ? `Último movimiento ${fmtWhen(state.lastMotion)}.` : "Todavía no hay movimientos registrados.";

  if (!online) {
    level = "offline";
    pill = "Sin conexión";
    title = "El aparato no responde";
    detail = lastSeen
      ? `Último contacto ${fmtWhen(lastSeen)}. Verifique que tenga luz y WiFi.`
      : "Nunca se conectó. Revise el token y el WiFi en secrets.h.";
  } else if (d.alert_active) {
    level = "critical";
    pill = "Alerta activa";
    title = `Sin movimiento hace ${fmtDuration(silent ?? 0)}`;
    detail = "Conviene llamar o pasar a verificar que esté todo bien.";
  } else if (paused) {
    level = "paused";
    pill = "Fuera de horario";
    title = "Alerta en pausa";
    detail = `${lastText} El control se reanuda a las ${fmtHour(s.day_start_h)}.`;
  } else if (ratio >= CAUTION_RATIO) {
    level = "caution";
    pill = "Atención";
    title = "Actividad baja";
    detail = `${lastText} Se acerca al umbral de alerta.`;
  } else {
    level = "ok";
    pill = "Actividad normal";
    title = "Todo en orden";
    detail = lastText;
  }

  $("status").dataset.level = level;
  $("status-pill-text").textContent = pill;
  $("status-title").textContent = title;
  $("status-detail").textContent = detail;
}

function renderMetrics(todayCount) {
  const d = state.device;
  $("m-today").textContent = todayCount == null ? "—" : String(todayCount);

  const peak = state.hourly.reduce((best, r) => (Number(r.motions) > (best ? Number(best.motions) : 0) ? r : best), null);
  if (peak) {
    const h = new Date(peak.hour).getHours();
    $("m-peak").textContent = `${fmtHour(h)}`;
    $("m-peak-foot").textContent = `${peak.motions} movimientos`;
  } else {
    $("m-peak").textContent = "—";
    $("m-peak-foot").textContent = "sin datos en 24 h";
  }

  $("m-signal").textContent = signalQuality(d.rssi);
  $("m-signal-foot").textContent = d.rssi != null ? `${d.rssi} dBm` : "";

  if (d.last_seen) {
    const secs = Math.max(0, Math.round((Date.now() - new Date(d.last_seen).getTime()) / 1000));
    $("m-heartbeat").textContent = secs < 90 ? `Hace ${secs} s` : `Hace ${fmtDuration(secs / 60)}`;
  } else {
    $("m-heartbeat").textContent = "Nunca";
  }
}

function renderChart() {
  // 24 hourly buckets ending at the current hour, in local time.
  const now = new Date();
  now.setMinutes(0, 0, 0);
  const buckets = [];
  for (let i = 23; i >= 0; i--) buckets.push(new Date(now.getTime() - i * 3600 * 1000));

  const counts = new Map(state.hourly.map((r) => [new Date(r.hour).getTime(), Number(r.motions)]));
  const labels = buckets.map((d) => fmtHour(d.getHours()));
  const values = buckets.map((d) => counts.get(d.getTime()) || 0);
  const colors = buckets.map((d) => (inWindow(d.getHours(), state.settings) ? css("--accent") : css("--line")));
  $("chart-legend").hidden = !(state.settings && state.settings.use_day_window);

  if (state.chart) {
    state.chart.data.labels = labels;
    state.chart.data.datasets[0].data = values;
    state.chart.data.datasets[0].backgroundColor = colors;
    state.chart.update("none");
    return;
  }

  const ink3 = css("--ink-3");
  const line = css("--line");
  const dataFont = { family: css("--font-data"), size: 11 };
  state.chart = new Chart($("chart"), {
    type: "bar",
    data: {
      labels,
      datasets: [{ label: "Movimientos", data: values, backgroundColor: colors, borderRadius: 3, maxBarThickness: 22 }],
    },
    options: {
      maintainAspectRatio: false,
      animation: { duration: 300 },
      plugins: {
        legend: { display: false },
        tooltip: {
          displayColors: false,
          callbacks: {
            title: (items) => `${items[0].label} – ${fmtHour((Number(items[0].label.slice(0, 2)) + 1) % 24)}`,
            label: (item) => `${item.raw} movimientos`,
          },
        },
      },
      scales: {
        x: { ticks: { color: ink3, font: dataFont, maxRotation: 0, autoSkip: true, maxTicksLimit: 8 }, grid: { display: false }, border: { color: line } },
        y: { beginAtZero: true, ticks: { color: ink3, font: dataFont, precision: 0, maxTicksLimit: 5 }, grid: { color: line }, border: { display: false } },
      },
    },
  });
}

// Re-color the chart when the OS theme changes.
window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", () => {
  if (state.chart) {
    state.chart.destroy();
    state.chart = null;
    renderChart();
  }
});

// ================================================================ settings

const form = $("settings-form");

for (const sel of [$("day-start"), $("day-end")]) {
  for (let h = 0; h < 24; h++) sel.add(new Option(fmtHour(h), String(h)));
}

function readForm() {
  return {
    inactivity_min: Number($("inactivity-h").value) * 60 + Number($("inactivity-m").value),
    repeat_min: Number($("repeat-min").value),
    notify_each: $("notify-each").checked,
    motion_gap_min: Number($("motion-gap").value),
    use_day_window: $("use-day-window").checked,
    day_start_h: Number($("day-start").value),
    day_end_h: Number($("day-end").value),
  };
}

function fillForm(s) {
  $("inactivity-h").value = Math.floor(s.inactivity_min / 60);
  $("inactivity-m").value = s.inactivity_min % 60;
  $("repeat-min").value = s.repeat_min;
  $("notify-each").checked = s.notify_each;
  $("motion-gap").value = s.motion_gap_min;
  $("use-day-window").checked = s.use_day_window;
  $("day-start").value = String(s.day_start_h);
  $("day-end").value = String(s.day_end_h);
  syncDependentFields();
  setFormState("Sin cambios", "");
}

function syncDependentFields() {
  const win = $("use-day-window").checked;
  $("day-start").disabled = !win;
  $("day-end").disabled = !win;
  $("motion-gap").disabled = !$("notify-each").checked;
}

function setFormState(text, kind) {
  const el = $("settings-msg");
  el.textContent = text;
  el.className = `form-state${kind ? ` is-${kind}` : ""}`;
}

function validate(v) {
  if (!Number.isInteger(v.inactivity_min) || v.inactivity_min < 1 || v.inactivity_min > 10080)
    return ["inactivity-h", "El tiempo sin movimiento debe estar entre 1 minuto y 7 días."];
  if (!Number.isInteger(Number($("inactivity-m").value)) || Number($("inactivity-m").value) > 59)
    return ["inactivity-m", "Los minutos deben estar entre 0 y 59."];
  if (!Number.isInteger(v.repeat_min) || v.repeat_min < 1 || v.repeat_min > 1440)
    return ["repeat-min", "La repetición debe estar entre 1 y 1440 minutos."];
  if (!Number.isInteger(v.motion_gap_min) || v.motion_gap_min < 0 || v.motion_gap_min > 1440)
    return ["motion-gap", "El intervalo debe estar entre 0 y 1440 minutos."];
  if (v.use_day_window && v.day_start_h === v.day_end_h)
    return ["day-start", "El horario activo debe empezar y terminar a horas distintas."];
  return null;
}

function isDirty() {
  if (!state.settings) return false;
  const v = readForm();
  return Object.keys(v).some((k) => v[k] !== state.settings[k]);
}

form.addEventListener("input", () => {
  syncDependentFields();
  for (const el of form.querySelectorAll("[aria-invalid]")) el.removeAttribute("aria-invalid");
  const dirty = isDirty();
  $("settings-save").disabled = !dirty;
  $("settings-reset").disabled = !dirty;
  setFormState(dirty ? "Cambios sin guardar" : "Sin cambios", dirty ? "dirty" : "");
});

$("settings-reset").addEventListener("click", () => {
  fillForm(state.settings);
  $("settings-save").disabled = true;
  $("settings-reset").disabled = true;
});

async function loadSettings() {
  const { data, error } = await sb.from("settings").select("*").eq("device_id", state.device.id).single();
  if (error) return setFormState(`No se pudo leer la configuración: ${error.message}`, "error");
  state.settings = data;
  fillForm(data);
}

form.addEventListener("submit", async (e) => {
  e.preventDefault();
  const v = readForm();
  const invalid = validate(v);
  if (invalid) {
    $(invalid[0]).setAttribute("aria-invalid", "true");
    $(invalid[0]).focus();
    return setFormState(invalid[1], "error");
  }

  $("settings-save").disabled = true;
  $("settings-reset").disabled = true;
  setFormState("Guardando…", "");
  const { data, error } = await sb
    .from("settings")
    .update({ ...v, updated_at: new Date().toISOString() })
    .eq("device_id", state.device.id)
    .select()
    .single();
  if (error) {
    $("settings-save").disabled = false;
    $("settings-reset").disabled = false;
    return setFormState(`No se pudo guardar: ${error.message}`, "error");
  }
  state.settings = data;
  fillForm(data);
  setFormState(`Guardado a las ${fmtClock(new Date())}`, "ok");
  renderStatus();
  renderChart();
});

// ================================================================ PWA

if ("serviceWorker" in navigator) {
  navigator.serviceWorker.register("sw.js").catch(() => {});
}
