(() => {
  const createEl = (tag, cls) => {
    const el = document.createElement(tag);
    if (cls) el.className = cls;
    return el;
  };

  const chipClass = (kind) => {
    if (!kind) return "chip";
    return `chip ${kind}`;
  };

  const buildTile = ({ title, value, meta, chip, secondary, disabled, ctaHref, ctaLabel }) => {
    const tile = createEl("div", `tile${disabled ? " disabled" : ""}`);
    const top = createEl("div", "tileRow");
    const t = createEl("div", "tileTitle");
    t.textContent = title || "—";
    const c = createEl("span", chipClass(chip));
    c.textContent = chip ? chip.toUpperCase() : "";
    top.append(t, c);
    const val = createEl("div", "tileValue");
    val.textContent = value || "—";
    const m = createEl("div", "tileMeta");
    m.textContent = meta || "";
    const s = createEl("div", "tileMeta");
    s.textContent = secondary || "";
    tile.append(top, val, m);
    if (secondary) tile.append(s);
    if (ctaHref && ctaLabel) {
      const link = createEl("a", "actionBtn");
      link.href = ctaHref;
      link.textContent = ctaLabel;
      tile.append(link);
    }
    return tile;
  };

  window.UI = { createEl, buildTile, chipClass };
})();
