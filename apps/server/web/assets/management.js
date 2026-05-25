(function () {
  "use strict";

  async function request(url, options) {
    const response = await fetch(url, options);
    const body = await response.json();
    if (!response.ok) {
      const error = new Error(body.error && body.error.message ? body.error.message : "Request failed");
      error.code = body.error && body.error.code ? body.error.code : "";
      throw error;
    }
    return body;
  }

  function escapeHtml(value) {
    return String(value == null || value === "" ? "-" : value).replace(/[&<>"']/g, function (character) {
      return {"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[character];
    });
  }

  function formatSystem(asset) {
    const parts = [asset.os, asset.os_version].filter(function (value) {
      return value != null && value !== "";
    });
    if (!parts.length) {
      return "-";
    }
    return parts.map(escapeHtml).join(" ");
  }

  function channelLabel(value) {
    return value === "stable" ? "稳定" :
      value === "candidate" ? "候选" :
      value === "dev" ? "开发" : value;
  }

  function platformLabel(value) {
    return value === "windows" ? "Windows" :
      value === "linux" ? "Linux" : value;
  }

  function agentStatusLabel(value) {
    return value === "online" ? "在线" :
      value === "offline" ? "离线" :
      value === "stale" ? "失联" : value;
  }

  function upgradeStateLabel(value) {
    return value === "queued" ? "已排队" :
      value === "waiting_reconnect" ? "等待重连" :
      value === "succeeded" ? "成功" :
      value === "failed" ? "失败" : value;
  }

  function packageStatusLabel(value) {
    return value === "published" ? "已发布" :
      value === "retired" ? "已退役" :
      value === "uploaded" ? "已上传" : value;
  }

  function buildTypeLabel(value) {
    return value === "release" ? "发布版" :
      value === "debug" ? "调试版" : value;
  }

  function localizeInstallError(error) {
    if (!error || !error.message) {
      return "请求失败";
    }
    if (error.code === "missing_required_field") {
      return "生成安装命令失败：缺少必要参数。";
    }
    if (error.code === "control_url_not_configured") {
      return "服务端未配置控制地址，暂时无法生成安装命令。";
    }
    if (error.code === "token_expiry_required") {
      return "生成注册凭证失败：缺少过期时间。";
    }
    return error.message;
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

  async function initInstall() {
    const output = document.querySelector("[data-install-token-output]");
    const command = document.querySelector("[data-install-command] code");
    const commandLabel = document.querySelector("[data-install-command-label]");
    const generateButton = document.querySelector("[data-generate-install-command]");
    const copyButton = document.querySelector("[data-copy-install]");
    const platform = document.querySelector("[data-install-platform]");
    const channel = document.querySelector("[data-install-channel]");

    function updateCommandLabel() {
      commandLabel.textContent = platformLabel(platform.value) + " / " +
        channelLabel(channel.value) + " 通道";
    }

    function resetCommandHint() {
      updateCommandLabel();
      command.textContent = "选择平台和发布通道后，点击“生成安装命令”。";
      output.textContent = "生成命令时会自动创建一次性注册凭证，明文不会单独展示给用户。";
      copyButton.disabled = true;
      setStatus("install", "准备就绪", false);
    }

    [platform, channel].forEach(function (node) {
      node.addEventListener("change", function () {
        resetCommandHint();
      });
    });

    copyButton.addEventListener("click", async function () {
      await copyText(command.textContent);
      setStatus("install", "命令已复制", true);
    });

    generateButton.addEventListener("click", async function () {
      try {
        updateCommandLabel();
        command.textContent = "正在生成安装命令...";
        output.textContent = "正在创建一次性注册凭证，请稍候。";
        copyButton.disabled = true;
        generateButton.disabled = true;
        const expiresAt = new Date(Date.now() + 60 * 60 * 1000).toISOString().replace(/\.\d{3}Z$/, "Z");
        const token = await request("/api/v1/install/tokens", {
          method: "POST",
          headers: {"content-type": "application/json"},
          body: JSON.stringify({purpose: "agent_register", expires_at: expiresAt, max_uses: 1})
        });
        const payload = await request("/api/v1/install/commands?server_url=" +
          encodeURIComponent(window.location.origin) +
          "&token=" + encodeURIComponent(token.token) +
          "&channel=" + encodeURIComponent(channel.value));
        command.textContent =
          platform.value === "windows" ? payload.commands.windows : payload.commands.linux;
        output.textContent = "命令中的注册凭证有效期至 " + token.expires_at + "，请尽快在目标机器执行。";
        copyButton.disabled = false;
        setStatus("install", "安装命令已生成", true);
      } catch (error) {
        const message = localizeInstallError(error);
        command.textContent = message;
        output.textContent = "生成失败后不会保留可复用凭证，请修正问题后重新生成。";
        copyButton.disabled = true;
        setStatus("install", message, false);
      } finally {
        generateButton.disabled = false;
      }
    });

    resetCommandHint();
  }

  async function initAgents() {
    const list = document.querySelector("[data-agent-list]");
    const filters = document.querySelector("[data-agent-filters]");
    const detail = document.querySelector("[data-agent-detail]");
    const detailBody = document.querySelector("[data-agent-detail-body]");
    const close = document.querySelector("[data-agent-close]");
    const backdrop = document.querySelector("[data-agent-backdrop]");
    const maintenanceDialog = document.querySelector("[data-maintenance-dialog]");
    const maintenanceAgent = document.querySelector("[data-maintenance-agent]");
    const maintenanceAction = document.querySelector("[data-maintenance-action]");
    const maintenancePackageRow = document.querySelector("[data-maintenance-package-row]");
    const maintenancePackage = document.querySelector("[data-maintenance-package]");
    const maintenanceConfirm = document.querySelector("[data-maintenance-confirm]");
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
      const packages = (await request("/api/v1/admin/packages")).packages;
      const asset = agent.latest_asset || {};
      const upgradePackages = packages.filter(function (item) {
        return item.component === "agent" && item.status !== "retired" &&
          item.platform === agent.platform && item.arch === asset.arch;
      });
      function openMaintenanceDialog(defaultAction) {
        maintenanceAgent.textContent = "目标 Agent: " + agentId;
        maintenancePackage.innerHTML = upgradePackages.map(function (item) {
          return '<option value="' + escapeHtml(item.package_id) + '">' +
            escapeHtml(item.version + " / " + item.build_type) + "</option>";
        }).join("");
        maintenanceAction.value = defaultAction || "upgrade";
        maintenancePackageRow.hidden =
          maintenanceAction.value !== "upgrade" || !upgradePackages.length;
        maintenanceDialog.showModal();
      }

      maintenanceAction.onchange = function () {
        maintenancePackageRow.hidden = maintenanceAction.value !== "upgrade";
      };
      maintenanceConfirm.onclick = async function (event) {
        event.preventDefault();
        try {
          if (maintenanceAction.value === "rollback") {
            await request("/api/v1/agents/" + encodeURIComponent(agentId) + "/rollback", {
              method: "POST",
              headers: {"content-type": "application/json"},
              body: JSON.stringify({set_by: "web-admin"})
            });
            setStatus("agents", "回滚任务已创建", true);
          } else {
            if (!maintenancePackage.value) {
              setStatus("agents", "没有可用升级版本，请先发布匹配包", false);
              return;
            }
            await request("/api/v1/agents/" + encodeURIComponent(agentId) + "/upgrade", {
              method: "POST",
              headers: {"content-type": "application/json"},
              body: JSON.stringify({package_id: maintenancePackage.value, set_by: "web-admin"})
            });
            setStatus("agents", "升级任务已创建", true);
          }
          maintenanceDialog.close();
          await show(agentId);
        } catch (error) {
          if (error.code === "capability_not_allowed") {
            setStatus("agents", "当前环境禁用了高风险写操作（allow_high_risk_write=false），无法升级/回滚。", false);
          } else {
            setStatus("agents", error.message, false);
          }
        }
      };

      const renderItems = function (items) {
        return items && items.length ? "<ul>" + items.map(function (item) {
          return "<li>" + escapeHtml(item) + "</li>";
        }).join("") + "</ul>" : '<p class="muted">暂无数据</p>';
      };
      const hasUpgradePackages = upgradePackages.length > 0;
      const maintenanceHint = hasUpgradePackages ?
        '<button class="button compact" data-open-maintenance>发起升级</button>' :
        '<button class="button compact" data-open-maintenance disabled>暂无可升级版本</button>';
      const rollbackAction = '<button class="button compact button-danger-text" data-open-maintenance data-maintenance-mode="rollback">执行回滚</button>';
      detailBody.innerHTML = "<h2>" + escapeHtml(asset.hostname || agent.agent_id) + "</h2>" +
        '<p class="detail-id">' + escapeHtml(agent.agent_id) + "</p>" +
        '<div class="drawer-section"><h3>运行状态</h3><dl>' +
        "<dt>状态</dt><dd>" + escapeHtml(agentStatusLabel(agent.status)) + "</dd>" +
        "<dt>系统</dt><dd>" + formatSystem(asset) + "</dd>" +
        "<dt>架构</dt><dd>" + escapeHtml(asset.arch) + "</dd>" +
        "<dt>最后在线</dt><dd>" + escapeHtml(agent.last_online_at) + "</dd>" +
        "<dt>最近错误</dt><dd>" + escapeHtml(agent.last_upgrade_error) + "</dd></dl></div>" +
        '<div class="drawer-section"><h3>版本信息</h3><dl>' +
        "<dt>当前版本</dt><dd>" + escapeHtml(agent.agent_version) + "</dd>" +
        "<dt>目标版本</dt><dd>" + escapeHtml(agent.desired_version) + "</dd>" +
        "<dt>升级状态</dt><dd>" + escapeHtml(upgradeStateLabel(agent.upgrade_state)) + "</dd></dl>" +
        '<div class="detail-actions">' + maintenanceHint + rollbackAction + "</div></div>" +
        "<h3>应用</h3>" + renderItems(asset.applications) +
        "<h3>服务</h3>" + renderItems(asset.services) +
        "<h3>资产快照</h3><p class=\"muted\">" + history.length + " 条记录，最近采集于 " +
        escapeHtml(asset.occurred_at) + "</p>";
      detail.setAttribute("aria-hidden", "false");
      backdrop.hidden = false;
      detailBody.querySelectorAll("[data-open-maintenance]").forEach(function (button) {
        button.addEventListener("click", function () {
          openMaintenanceDialog(button.dataset.maintenanceMode || "upgrade");
        });
      });
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
          escapeHtml(agent.status) + '">' + escapeHtml(agentStatusLabel(agent.status)) + "</span></td><td><strong>" +
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
        const stableButton = item.build_type === "debug" ? "" :
          '<button class="button" data-channel="stable">发布到稳定</button>';
        return '<section class="package-row"><div class="row-head"><strong>' + escapeHtml(item.component) +
          " " + escapeHtml(item.version) +
          '</strong><span class="tag">' + escapeHtml(packageStatusLabel(item.status)) + '</span></div><span class="muted">' +
          escapeHtml(platformLabel(item.platform)) + " / " + escapeHtml(item.arch) + " / " + escapeHtml(buildTypeLabel(item.build_type)) +
          '</span><div class="release-actions" data-package="' + escapeHtml(item.package_id) + '">' +
          stableButton + '<button class="button" data-channel="candidate">发布到候选</button>' +
          '<button class="button" data-channel="dev">发布到开发</button>' +
          '<button class="button" data-retire>退役</button></div></section>';
      }).join("") || '<p class="muted">尚无已上传安装包。</p>';
      list.querySelectorAll("[data-channel]").forEach(function (button) {
        button.addEventListener("click", async function () {
          const id = button.parentElement.dataset.package;
          await request("/api/v1/admin/packages/" + id + "/publish", {
            method: "POST", headers: {"content-type": "application/json"},
            body: JSON.stringify({channel: button.dataset.channel})
          });
          setStatus("packages", "已发布到" + channelLabel(button.dataset.channel) + "通道", true);
          await refresh();
        });
      });
      list.querySelectorAll("[data-retire]").forEach(function (button) {
        button.addEventListener("click", async function () {
          const id = button.parentElement.dataset.package;
          await request("/api/v1/admin/packages/" + id + "/retire", {method: "POST"});
          setStatus("packages", "安装包已退役", true);
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
