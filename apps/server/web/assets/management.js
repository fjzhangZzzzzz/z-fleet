(function () {
  "use strict";

  async function request(url, options) {
    const response = await fetch(url, options);
    const body = await response.json();
    if (!response.ok) {
      throw new Error(body.error && body.error.message ? body.error.message : "Request failed");
    }
    return body;
  }

  function escapeText(value) {
    return String(value == null ? "-" : value);
  }

  function setStatus(page, text, ok) {
    const node = document.querySelector('[data-status-for="' + page + '"]');
    if (!node) return;
    node.textContent = text;
    node.classList.toggle("ok", Boolean(ok));
  }

  async function initInstall() {
    const fields = document.querySelector("[data-install-fields]");
    const release = document.querySelector("[data-install-release]");
    const output = document.querySelector("[data-install-token-output]");
    const command = document.querySelector("[data-install-command] code");
    const button = document.querySelector("[data-install-token-button]");
    async function refresh() {
      const values = new FormData(fields);
      const query = new URLSearchParams(values).toString();
      const options = await request("/api/v1/install/options?" + query);
      release.innerHTML = "<dt>Published version</dt><dd>" + escapeText(options.agent_version) +
        "</dd><dt>SHA-256</dt><dd>" + escapeText(options.sha256) + "</dd>";
      command.textContent = options.download_url ?
        "zfleet_agent install --channel " + values.get("channel") + " --token <register-token>" :
        "当前 channel 尚无已发布安装包。";
    }
    fields.addEventListener("change", function () { refresh().catch(function (error) { setStatus("install", error.message, false); }); });
    button.addEventListener("click", async function () {
      try {
        const values = Object.fromEntries(new FormData(fields).entries());
        const token = await request("/api/v1/install/tokens", {
          method: "POST",
          headers: {"content-type": "application/json"},
          body: JSON.stringify(values)
        });
        output.textContent = token.token;
        command.textContent = "zfleet_agent install --channel " + values.channel + " --token " + token.token;
        setStatus("install", "Token 已生成", true);
      } catch (error) {
        setStatus("install", error.message, false);
      }
    });
    await refresh();
  }

  async function initAgents() {
    const list = document.querySelector("[data-agent-list]");
    const detail = document.querySelector("[data-agent-detail]");
    const filter = document.querySelector("[data-agent-filter]");
    let agents = [];
    async function show(agentId) {
      const agent = await request("/api/v1/agents/" + encodeURIComponent(agentId));
      const asset = agent.latest_asset || {};
      detail.innerHTML = "<h2>" + escapeText(agent.agent_id) + "</h2><dl>" +
        "<dt>Status</dt><dd>" + escapeText(agent.status) + "</dd>" +
        "<dt>Hostname</dt><dd>" + escapeText(asset.hostname) + "</dd>" +
        "<dt>OS / Arch</dt><dd>" + escapeText(asset.os) + " / " + escapeText(asset.arch) + "</dd>" +
        "<dt>Version</dt><dd>" + escapeText(agent.agent_version) + "</dd>" +
        "<dt>Last seen</dt><dd>" + escapeText(agent.last_seen_at) + "</dd></dl>";
    }
    function render() {
      const needle = filter.value.toLowerCase();
      const visible = agents.filter(function (agent) { return agent.agent_id.toLowerCase().includes(needle); });
      list.innerHTML = visible.map(function (agent) {
        return '<div class="agent-row" data-agent="' + agent.agent_id + '"><div class="row-head"><strong>' +
          escapeText(agent.agent_id) + '</strong><span class="tag">' + escapeText(agent.status) +
          "</span></div><span class=\"muted\">" + escapeText(agent.platform) + " | " +
          escapeText(agent.last_seen_at) + "</span></div>";
      }).join("") || '<p class="muted">没有匹配的 Agent。</p>';
      list.querySelectorAll("[data-agent]").forEach(function (row) {
        row.addEventListener("click", function () { show(row.dataset.agent); });
      });
    }
    agents = (await request("/api/v1/agents")).agents;
    filter.addEventListener("input", render);
    render();
    if (agents.length) await show(agents[0].agent_id);
  }

  async function initPackages() {
    const form = document.querySelector("[data-package-upload-form]");
    const list = document.querySelector("[data-package-list]");
    async function refresh() {
      const packages = (await request("/api/v1/admin/packages")).packages;
      list.innerHTML = packages.map(function (item) {
        return '<section class="package-row"><div class="row-head"><strong>' + escapeText(item.version) +
          '</strong><span class="tag">' + escapeText(item.status) + '</span></div><span class="muted">' +
          escapeText(item.platform) + " / " + escapeText(item.arch) +
          '</span><div class="release-actions" data-package="' + item.package_id + '">' +
          '<button class="button" data-channel="stable">stable</button><button class="button" data-channel="candidate">candidate</button>' +
          '<button class="button" data-channel="dev">dev</button></div></section>';
      }).join("") || '<p class="muted">尚无已上传安装包。</p>';
      list.querySelectorAll("[data-channel]").forEach(function (button) {
        button.addEventListener("click", async function () {
          const id = button.parentElement.dataset.package;
          await request("/api/v1/admin/packages/" + id + "/publish", {
            method: "POST", headers: {"content-type": "application/json"},
            body: JSON.stringify({channel: button.dataset.channel})
          });
          setStatus("packages", "已发布到 " + button.dataset.channel, true);
          await refresh();
        });
      });
    }
    form.addEventListener("submit", async function (event) {
      event.preventDefault();
      try {
        const data = new FormData(form);
        const file = data.get("package");
        const query = new URLSearchParams({platform: data.get("platform"), arch: data.get("arch"), filename: file.name});
        await request("/api/v1/admin/packages?" + query.toString(), {method: "POST", body: file});
        setStatus("packages", "校验通过", true);
        await refresh();
      } catch (error) {
        setStatus("packages", error.message, false);
      }
    });
    await refresh();
  }

  const initializers = {install: initInstall, agents: initAgents, packages: initPackages};
  const initialize = initializers[document.body.dataset.page];
  if (initialize) {
    initialize().catch(function (error) { setStatus(document.body.dataset.page, error.message, false); });
  }
}());
