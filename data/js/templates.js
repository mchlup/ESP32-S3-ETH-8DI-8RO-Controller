const Templates = {
  "MixingValve3Point": {
    render(container, values) {
      container.innerHTML += `
        <label>Čas chodu (s): <input type="number" id="travelTime" value="${values.travelTime ?? 6}" /></label>
        <label>Invertovat směr <input type="checkbox" id="invert" ${values.invert ? "checked" : ""}></label>
      `;
    },
    collect(container) {
      return {
        travelTime: parseFloat(container.querySelector("#travelTime").value),
        invert: container.querySelector("#invert").checked
      };
    }
  },
  "ThermostatInput": {
    render(container, values) {
      container.innerHTML += `
        <label>Hystereze (°C): <input type="number" step="0.1" id="hysteresis" value="${values.hysteresis ?? 0.5}" /></label>
      `;
    },
    collect(container) {
      return {
        hysteresis: parseFloat(container.querySelector("#hysteresis").value)
      };
    }
  },
  "HeaterStage": {
    render(container, values) {
      container.innerHTML += `
        <label>Max teplota (°C): <input type="number" id="maxTemp" value="${values.maxTemp ?? 70}" /></label>
      `;
    },
    collect(container) {
      return {
        maxTemp: parseFloat(container.querySelector("#maxTemp").value)
      };
    }
  }
};
