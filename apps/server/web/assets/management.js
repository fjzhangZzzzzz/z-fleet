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

  function escapeHtml(value) {
    return String(value == null || value === "" ? "-" : value).replace(/[&<>"']/g, function (character) {
      return {"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[character];
    });
  }

  function setStatus(page, text, ok) {
    const node = document.querySelector('[data-status-for="' + page + '"]');
    if (!node) return;
    node.textContent = text;
    node.classList.toggle("ok", Boolean(ok));
  }

  async function copyText(value) {
    await navigator.clipboard.writeText(value);
  }

  function shellQuote(value) {
    return "'" + String(value).replace(/'/g, "'\"'\"'") + "'";
  }

  async function initInstall() {
    const release = document.querySelector("[data-install-release]");
    const output = document.querySelector("[data-install-token-output]");
    const command = document.querySelector("[data-install-command] code");
    const tokenButton = document.querySelector("[data-install-token-button]");
    const copyButton = document.querySelector("[data-copy-install]");
    const controlUrl = document.querySelector("[data-control-url]");
    let options;
    let tokenValue = "";

    function renderCommand() {
      if (!options || !options.download_url) {
        command.textContent = "当前没有可部署的推荐版本。";
        copyButton.disabled = true;
        return;
      }
      if (!tokenValue) {
        command.textContent = "生成注册 token 后显示可执行命令。";
        copyButton.disabled = true;
        return;
      }
      const packageUrl = window.location.origin + options.download_url;
      const commandText = "curl -fL " + shellQuote(packageUrl) + " -o /tmp/zfleet-agent.zip && \\\n" +
        "sudo zfleet_installer apply --root /opt --package /tmp/zfleet-agent.zip && \\\n" +
        "sudo env ZFLEET_COMPONENT_ROOT=/opt/zfleet/agent /opt/zfleet/agent/releases/" +
        options.agent_version + "/bin/zfleet_agent --control-url " + shellQuote(controlUrl.value) +
        " --registration-token " + shellQuote(tokenValue);
      command.textContent = commandText;
      copyButton.disabled = false;
    }

    async function refresh() {
      options = await request("/api/v1/install/options");
      release.innerHTML = "<dt>推荐版本</dt><dd>" + escapeHtml(options.agent_version) +
        "</dd><dt>SHA-256</dt><dd>" + escapeHtml(options.sha256) + "</dd>";
      renderCommand();
    }

    controlUrl.addEventListener("input", renderCommand);
    copyButton.addEventListener("click", async function () {
      await copyText(command.textContent);
      setStatus("install", "命令已复制", true);
    });
    tokenButton.addEventListener("click", async function () {
      try {
        const expiresAt = new Date(Date.now() + 60 * 60 * 1000).toISOString().replace(/\.\d{3}Z$/, "Z");
        const token = await request("/api/v1/install/tokens", {
          method: "POST",
          headers: {"content-type": "application/json"},
          body: JSON.stringify({purpose: "agent_register", expires_at: expiresAt, max_uses: 1})
        });
        tokenValue = token.token;
        output.textContent = "有效期至 " + token.expires_at + "，明文仅展示本次。";
        renderCommand();
        setStatus("install", "Token 已生成", true);
      } catch (error) {
        setStatus("install", error.message, false);
      }
    });
    await refresh();
  }

  async function initAgents() {
    const list = document.querySelector("[data-agent-list]");
    const filters = document.querySelector("[data-agent-filters]");
    const detail = document.querySelector("[data-agent-detail]");
    const detailBody = document.querySelector("[data-agent-detail-body]");
    const close = document.querySelector("[data-agent-close]");
    const backdrop = document.querySelector("[data-agent-backdrop]");
    let agents = [];

    function assetOf(agent) {
      return agent.latest_asset || {};
    }

    function valuesFor(field) {
      const values = agents.map(function (agent) {
        const asset = assetOf(agent);
        return field === "status" ? agent.status :
          field === "version" ? agent.agent_version : asset[field];
      }).filter(Boolean);
      return Array.from(new Set(values)).sort();
    }

    function fillFilter(field) {
      const select = filters.querySelector('[name="' + field + '"]');
      const label = select.options[0].textContent;
      select.innerHTML = '<option value="">' + label + "</option>" +
        valuesFor(field).map(function (value) {
          return '<option value="' + escapeHtml(value) + '">' + escapeHtml(value) + "</option>";
        }).join("");
    }

    async function show(agentId) {
      const agent = await request("/api/v1/agents/" + encodeURIComponent(agentId));
      const history = (await request("/api/v1/agents/" + encodeURIComponent(agentId) + "/assets")).assets;
      const asset = agent.latest_asset || {};
      const renderItems = function (items) {
        return items && items.length ? "<ul>" + items.map(function (item) {
          return "<li>" + escapeHtml(item) + "</li>";
        }).join("") + "</ul>" : '<p class="muted">暂无数据</p>';
      };
      detailBody.innerHTML = "<h2>" + escapeHtml(asset.hostname || agent.agent_id) + "</h2>" +
        '<p class="detail-id">' + escapeHtml(agent.agent_id) + "</p><dl>" +
        "<dt>状态</dt><dd>" + escapeHtml(agent.status) + "</dd>" +
        "<dt>系统</dt><dd>" + escapeHtml(asset.os) + " " + escapeHtml(asset.os_version) + "</dd>" +
        "<dt>架构</dt><dd>" + escapeHtml(asset.arch) + "</dd>" +
        "<dt>版本</dt><dd>" + escapeHtml(agent.agent_version) + "</dd>" +
        "<dt>最后在线</dt><dd>" + escapeHtml(agent.last_online_at) + "</dd></dl>" +
        "<h3>应用</h3>" + renderItems(asset.applications) +
        "<h3>服务</h3>" + renderItems(asset.services) +
        "<h3>资产快照</h3><p class=\"muted\">" + history.length + " 条记录，最近采集于 " +
        escapeHtml(asset.occurred_at) + "</p>";
      detail.setAttribute("aria-hidden", "false");
      backdrop.hidden = false;
    }

    function closeDetail() {
      detail.setAttribute("aria-hidden", "true");
      backdrop.hidden = true;
    }

    function readFilterQuery() {
      return {
        search: filters.querySelector('[name="search"]').value || "",
        status: filters.querySelector('[name="status"]').value || "",
        os: filters.querySelector('[name="os"]').value || "",
        arch: filters.querySelector('[name="arch"]').value || "",
        version: filters.querySelector('[name="version"]').value || "",
      };
    }

    function render() {
      const query = readFilterQuery();
      const needle = query.search.toLowerCase();
      const visible = agents.filter(function (agent) {
        const asset = assetOf(agent);
        return (!needle || (agent.agent_id + " " + (asset.hostname || "")).toLowerCase().includes(needle)) &&
          (!query.status || agent.status === query.status) &&
          (!query.os || asset.os === query.os) &&
          (!query.arch || asset.arch === query.arch) &&
          (!query.version || agent.agent_version === query.version);
      });
      list.innerHTML = visible.map(function (agent) {
        const asset = assetOf(agent);
        return '<tr class="agent-row" data-agent="' + escapeHtml(agent.agent_id) + '"><td><span class="tag ' +
          escapeHtml(agent.status) + '">' + escapeHtml(agent.status) + "</span></td><td><strong>" +
          escapeHtml(asset.hostname) + "</strong></td><td>" + escapeHtml(asset.os) + "</td><td>" +
          escapeHtml(asset.arch) + "</td><td>" + escapeHtml(agent.agent_version) + "</td><td class=\"mono\">" +
          escapeHtml(agent.agent_id) + "</td><td>" + escapeHtml(agent.last_online_at) + "</td></tr>";
      }).join("") || '<tr><td colspan="7" class="muted">没有匹配的 Agent。</td></tr>';
      list.querySelectorAll("[data-agent]").forEach(function (row) {
        row.addEventListener("click", function () { show(row.dataset.agent); });
      });
    }

    agents = (await request("/api/v1/agents")).agents;
    ["status", "os", "arch", "version"].forEach(fillFilter);
    filters.addEventListener("input", render);
    filters.addEventListener("change", render);
    close.addEventListener("click", closeDetail);
    backdrop.addEventListener("click", closeDetail);
    render();
  }

  async function initPackages() {
    const form = document.querySelector("[data-package-upload-form]");
    const list = document.querySelector("[data-package-list]");
    async function refresh() {
      const packages = (await request("/api/v1/admin/packages")).packages;
      list.innerHTML = packages.map(function (item) {
        return '<section class="package-row"><div class="row-head"><strong>' + escapeHtml(item.version) +
          '</strong><span class="tag">' + escapeHtml(item.status) + '</span></div><span class="muted">' +
          escapeHtml(item.platform) + " / " + escapeHtml(item.arch) +
          '</span><div class="release-actions" data-package="' + escapeHtml(item.package_id) + '">' +
          '<button class="button" data-channel="stable">发布 stable</button><button class="button" data-channel="candidate">发布 candidate</button>' +
          '<button class="button" data-channel="dev">发布 dev</button></div></section>';
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
        const file = new FormData(form).get("package");
        const query = new URLSearchParams({filename: file.name});
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
