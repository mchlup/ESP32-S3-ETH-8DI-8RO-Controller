document.addEventListener("DOMContentLoaded", () => {
  const theme = localStorage.getItem("theme") || "light";
  document.documentElement.setAttribute("data-theme", theme);
  document.getElementById("themeToggle").addEventListener("click", () => {
    const newTheme = theme === "light" ? "dark" : "light";
    document.documentElement.setAttribute("data-theme", newTheme);
    localStorage.setItem("theme", newTheme);
  });

  loadConfig();
  setInterval(() => {
    loadStatus();
    renderTest();
  }, 1000);
  document.getElementById("saveConfig").addEventListener("click", saveConfig);
});

let currentConfig = { inputs: [], outputs: [] };

function loadConfig() {
  fetch("/api/config").then(res => res.json()).then(cfg => {
    currentConfig = cfg;
    const container = document.getElementById("io-config-container");
    container.innerHTML = "";
    (cfg.inputs || []).forEach(io => renderConfig(container, io));
    (cfg.outputs || []).forEach(io => renderConfig(container, io));
  });
}

function renderConfig(container, io) {
  const wrap = document.createElement("div");
  wrap.className = "config-block";
  wrap.dataset.ioId = io.id;
  wrap.innerHTML = `<h3>${io.name} (${io.type})</h3>`;
  const template = Templates[io.template];
  if (template) template.render(wrap, io.params || {});
  container.appendChild(wrap);
}

function loadStatus() {
  fetch("/api/status").then(res => res.json()).then(stat => {
    const box = document.getElementById("status-container");
    box.innerHTML = "";
    (stat.inputs || []).forEach(s => {
      const div = document.createElement("div");
      div.textContent = `🔌 Vstup ${s.id}: ${s.state ? "AKTIVNÍ" : "neaktivní"}`;
      box.appendChild(div);
    });
    (stat.outputs || []).forEach(s => {
      const div = document.createElement("div");
      div.textContent = `⚡ Výstup ${s.id}: ${s.state ? "ZAP" : "vypnuto"}`;
      box.appendChild(div);
    });
  });
}

function renderTest() {
  const box = document.getElementById("test-container");
  box.innerHTML = "";
  (currentConfig.outputs || []).forEach(out => {
    const btn = document.createElement("button");
    btn.textContent = `Přepnout ${out.name}`;
    btn.onclick = () => {
      fetch("/api/output/set", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ id: out.id, toggle: true })
      });
    };
    box.appendChild(btn);
  });
}

function saveConfig() {
  const updated = { inputs: [], outputs: [] };
  [...document.querySelectorAll("[data-io-id]")].forEach(block => {
    const id = block.dataset.ioId;
    const io = (currentConfig.inputs || []).find(i => i.id === id) || (currentConfig.outputs || []).find(o => o.id === id);
    const params = Templates[io.template]?.collect(block) || {};
    const target = { ...io, params };
    (io.type === "DI" ? updated.inputs : updated.outputs).push(target);
  });
  fetch("/api/config", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(updated)
  }).then(() => alert("Konfigurace uložena."));
}
