(function () {
  "use strict";

  function $(selector, scope) {
    return (scope || document).querySelector(selector);
  }

  function $$(selector, scope) {
    return Array.from((scope || document).querySelectorAll(selector));
  }

  async function request(url, options) {
    const response = await fetch(url, options);
    const body = await response.json();
    if (!response.ok) {
      const code = body.error_code || "";
      const message = body.message || "Request failed";
      const error = new Error(message);
      error.code = code;
      throw error;
    }
    return body;
  }

  function escapeHtml(value) {
    return String(value == null || value === "" ? "-" : value).replace(/[&<>"']/g, function (character) {
      return {"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[character];
    });
  }

  function setStatus(page, text, ok) {
    const node = $('[data-status-for="' + page + '"]');
    if (!node) return;
    node.textContent = text;
    node.classList.toggle("ok", Boolean(ok));
  }

  function channelLabel(value) {
    return value === "stable" ? "稳定通道" :
      value === "candidate" ? "候选通道" :
      value === "dev" ? "开发通道" : value;
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

  function packageStatusLabel(value) {
    return value === "published" ? "已发布" :
      value === "validated" ? "已校验" :
      value === "retired" ? "已退役" :
      value === "uploaded" ? "已上传" : value;
  }

  function upgradeStateLabel(value) {
    return value === "queued" ? "已排队" :
      value === "waiting_reconnect" ? "等待重连" :
      value === "succeeded" ? "已成功" :
      value === "failed" ? "已失败" : "-";
  }

  function buildTypeLabel(value) {
    return value === "release" ? "发布版" :
      value === "debug" ? "调试版" : value;
  }

  function componentLabel(value) {
    return value === "agent" ? "Agent" :
      value === "installer" ? "安装器" : value;
  }

  function formatSystem(asset) {
    const parts = [asset.os, asset.os_version].filter(Boolean);
    return parts.length ? parts.map(escapeHtml).join(" ") : "-";
  }

  function formatAbsoluteTime(value) {
    if (!value) return "-";
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return value;
    const year = date.getFullYear();
    const month = String(date.getMonth() + 1).padStart(2, "0");
    const day = String(date.getDate()).padStart(2, "0");
    const hours = String(date.getHours()).padStart(2, "0");
    const minutes = String(date.getMinutes()).padStart(2, "0");
    const seconds = String(date.getSeconds()).padStart(2, "0");
    return year + "-" + month + "-" + day + " " + hours + ":" + minutes + ":" + seconds;
  }

  function formatRelativeTime(value) {
    if (!value) return "";
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return "";
    const diff = Date.now() - date.getTime();
    const past = diff >= 0;
    const minutes = Math.round(Math.abs(diff) / 60000);
    if (minutes < 1) return past ? "刚刚" : "即将";
    if (minutes < 60) return (past ? "" : "约 ") + minutes + " 分钟" + (past ? "前" : "后");
    const hours = Math.round(minutes / 60);
    if (hours < 24) return (past ? "" : "约 ") + hours + " 小时" + (past ? "前" : "后");
    const days = Math.round(hours / 24);
    return (past ? "" : "约 ") + days + " 天" + (past ? "前" : "后");
  }

  function formatLocalTime(value) {
    if (!value) return "-";
    const absolute = formatAbsoluteTime(value);
    const relative = formatRelativeTime(value);
    return relative ? absolute + "（" + relative + "）" : absolute;
  }

  function localizeInstallError(error) {
    if (!error || !error.message) return "请求失败";
    if (error.code === "missing_required_field") return "生成安装命令失败：缺少必要参数。";
    if (error.code === "control_public_url_not_configured") return "服务端未配置公网控制地址，暂时无法生成安装命令。";
    if (error.code === "token_expiry_required") return "生成注册凭证失败：缺少过期时间。";
    return error.message;
  }

  function renderChoiceGroup(nodes, currentValue) {
    nodes.forEach(function (node) {
      const active = node.value === currentValue;
      node.classList.toggle("active", active);
      node.setAttribute("aria-pressed", active ? "true" : "false");
    });
  }

  function renderFilterGroup(container, items, currentValue, labelFormatter) {
    const formatter = labelFormatter || function (value) { return value; };
    container.innerHTML = [
      '<button class="filter-chip' + (currentValue === "" ? " active" : "") + '" type="button" data-value="">全部</button>'
    ].concat(items.map(function (item) {
      return '<button class="filter-chip' + (item === currentValue ? " active" : "") +
        '" type="button" data-value="' + escapeHtml(item) + '">' + escapeHtml(formatter(item)) + "</button>";
    })).join("");
  }

  async function copyText(value) {
    await navigator.clipboard.writeText(value);
  }

  async function initInstall() {
    const output = $("[data-install-token-output]");
    const command = $("[data-install-command] code");
    const commandLabel = $("[data-install-command-label]");
    const generateButton = $("[data-generate-install-command]");
    const copyButton = $("[data-copy-install]");
    const platformButtons = $$("[data-install-platform]");
    const channelButtons = $$("[data-install-channel]");
    const state = {platform: "linux", channel: "stable"};

    function updateLabel() {
      commandLabel.textContent = platformLabel(state.platform) + " · " + channelLabel(state.channel);
    }

    function resetHint() {
      updateLabel();
      command.textContent = "选择平台和发布通道后，点击“生成安装命令”。";
      output.textContent = "生成命令时会自动创建一次性注册凭证，明文不会单独展示给用户。";
      copyButton.disabled = true;
      setStatus("install", "", false);
    }

    platformButtons.forEach(function (button) {
      button.addEventListener("click", function () {
        state.platform = button.value;
        renderChoiceGroup(platformButtons, state.platform);
        resetHint();
      });
    });
    channelButtons.forEach(function (button) {
      button.addEventListener("click", function () {
        state.channel = button.value;
        renderChoiceGroup(channelButtons, state.channel);
        resetHint();
      });
    });

    copyButton.addEventListener("click", async function () {
      await copyText(command.textContent);
      setStatus("install", "安装命令已复制", true);
    });

    generateButton.addEventListener("click", async function () {
      try {
        updateLabel();
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
          "&channel=" + encodeURIComponent(state.channel) +
          "&platform=" + encodeURIComponent(state.platform));
        command.textContent = payload.command;
        output.textContent = "凭证有效至 " + formatAbsoluteTime(token.expires_at) + "，请尽快在目标机器执行。";
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

    renderChoiceGroup(platformButtons, state.platform);
    renderChoiceGroup(channelButtons, state.channel);
    resetHint();
  }

  async function initAgents() {
    const list = $("[data-agent-list]");
    const filters = $("[data-agent-filters]");
    const detail = $("[data-agent-detail]");
    const detailBody = $("[data-agent-detail-body]");
    const close = $("[data-agent-close]");
    const backdrop = $("[data-agent-backdrop]");
    const maintenanceDialog = $("[data-maintenance-dialog]");
    const maintenanceAgent = $("[data-maintenance-agent]");
    const maintenanceAction = $("[data-maintenance-action]");
    const maintenancePackageRow = $("[data-maintenance-package-row]");
    const maintenancePackage = $("[data-maintenance-package]");
    const maintenanceConfirm = $("[data-maintenance-confirm]");
    const resetButton = $("[data-reset-agent-filters]");
    const versionTrigger = $("[data-version-trigger]");
    const versionMenu = $("[data-version-menu]");
    let agents = [];
    const state = {status: "", os: "", arch: "", version: "", search: ""};

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

    function renderFilters() {
      renderFilterGroup($('[data-filter-group="status"]', filters), valuesFor("status"), state.status, agentStatusLabel);
      renderFilterGroup($('[data-filter-group="os"]', filters), valuesFor("os"), state.os);
      renderFilterGroup($('[data-filter-group="arch"]', filters), valuesFor("arch"), state.arch);
      const versions = valuesFor("version");
      versionTrigger.textContent = state.version || "全部版本";
      versionMenu.innerHTML = [
        '<button class="select-option' + (state.version === "" ? " active" : "") + '" type="button" data-version-value="">全部版本</button>'
      ].concat(versions.map(function (value) {
        return '<button class="select-option' + (state.version === value ? " active" : "") +
          '" type="button" data-version-value="' + escapeHtml(value) + '">' + escapeHtml(value) + "</button>";
      })).join("");
    }

    function bindFilterEvents() {
      ["status", "os", "arch"].forEach(function (field) {
        const container = $('[data-filter-group="' + field + '"]', filters);
        container.addEventListener("click", function (event) {
          const button = event.target.closest("[data-value]");
          if (!button) return;
          state[field] = button.dataset.value || "";
          renderFilters();
          render();
        });
      });
      $('[name="search"]', filters).addEventListener("input", function (event) {
        state.search = event.target.value || "";
        render();
      });
      versionTrigger.addEventListener("click", function () {
        const expanded = versionTrigger.getAttribute("aria-expanded") === "true";
        versionTrigger.setAttribute("aria-expanded", expanded ? "false" : "true");
        versionMenu.hidden = expanded;
      });
      versionMenu.addEventListener("click", function (event) {
        const button = event.target.closest("[data-version-value]");
        if (!button) return;
        state.version = button.dataset.versionValue || "";
        versionTrigger.setAttribute("aria-expanded", "false");
        versionMenu.hidden = true;
        renderFilters();
        render();
      });
      document.addEventListener("click", function (event) {
        if (!filters.contains(event.target)) {
          versionTrigger.setAttribute("aria-expanded", "false");
          versionMenu.hidden = true;
        }
      });
      resetButton.addEventListener("click", function () {
        state.status = "";
        state.os = "";
        state.arch = "";
        state.version = "";
        state.search = "";
        $('[name="search"]', filters).value = "";
        renderFilters();
        render();
      });
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
        maintenanceAgent.textContent = "目标 Agent：" + agentId;
        maintenancePackage.innerHTML = upgradePackages.map(function (item) {
          return '<option value="' + escapeHtml(item.package_id) + '">' +
            escapeHtml(item.version + " / " + buildTypeLabel(item.build_type)) + "</option>";
        }).join("");
        maintenanceAction.value = defaultAction || "upgrade";
        maintenancePackageRow.hidden = maintenanceAction.value !== "upgrade" || !upgradePackages.length;
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
              setStatus("agents", "没有可用升级版本，请先发布匹配安装包。", false);
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
            setStatus("agents", "当前环境禁用了高风险写操作，无法升级或回滚。", false);
          } else {
            setStatus("agents", error.message, false);
          }
        }
      };

      function renderItems(items) {
        return items && items.length ? "<ul>" + items.map(function (item) {
          return "<li>" + escapeHtml(item) + "</li>";
        }).join("") + "</ul>" : '<p class="muted">暂无数据</p>';
      }

      const maintenanceHint = upgradePackages.length ?
        '<button class="button compact" data-open-maintenance>发起升级</button>' :
        '<button class="button compact" data-open-maintenance disabled>暂无可升级版本</button>';
      const rollbackAction = '<button class="button compact button-danger-text" data-open-maintenance data-maintenance-mode="rollback">执行回滚</button>';

      detailBody.innerHTML = "<h2>" + escapeHtml(asset.hostname || agent.agent_id) + "</h2>" +
        '<p class="detail-id">' + escapeHtml(agent.agent_id) + "</p>" +
        '<div class="drawer-section"><h3>运行状态</h3><dl>' +
        "<dt>状态</dt><dd>" + escapeHtml(agentStatusLabel(agent.status)) + "</dd>" +
        "<dt>系统</dt><dd>" + formatSystem(asset) + "</dd>" +
        "<dt>架构</dt><dd>" + escapeHtml(asset.arch) + "</dd>" +
        "<dt>最后在线</dt><dd>" + escapeHtml(formatLocalTime(agent.last_online_at)) + "</dd>" +
        "<dt>最近错误</dt><dd>" + escapeHtml(agent.last_upgrade_error) + "</dd></dl></div>" +
        '<div class="drawer-section"><h3>版本信息</h3><dl>' +
        "<dt>当前版本</dt><dd>" + escapeHtml(agent.agent_version) + "</dd>" +
        "<dt>目标版本</dt><dd>" + escapeHtml(agent.desired_version) + "</dd>" +
        "<dt>升级状态</dt><dd>" + escapeHtml(upgradeStateLabel(agent.upgrade_state)) + "</dd></dl>" +
        '<div class="detail-actions">' + maintenanceHint + rollbackAction + "</div></div>" +
        "<h3>应用</h3>" + renderItems(asset.applications) +
        "<h3>服务</h3>" + renderItems(asset.services) +
        "<h3>资产快照</h3><p class=\"muted\">" + history.length + " 条记录，最近采集于 " +
        escapeHtml(formatLocalTime(asset.occurred_at)) + "</p>";
      detail.setAttribute("aria-hidden", "false");
      backdrop.hidden = false;
      $$("[data-open-maintenance]", detailBody).forEach(function (button) {
        button.addEventListener("click", function () {
          openMaintenanceDialog(button.dataset.maintenanceMode || "upgrade");
        });
      });
    }

    function closeDetail() {
      detail.setAttribute("aria-hidden", "true");
      backdrop.hidden = true;
    }

    function render() {
      const needle = state.search.toLowerCase();
      const visible = agents.filter(function (agent) {
        const asset = assetOf(agent);
        return (!needle || (agent.agent_id + " " + (asset.hostname || "")).toLowerCase().includes(needle)) &&
          (!state.status || agent.status === state.status) &&
          (!state.os || asset.os === state.os) &&
          (!state.arch || asset.arch === state.arch) &&
          (!state.version || agent.agent_version === state.version);
      });
      list.innerHTML = visible.map(function (agent) {
        const asset = assetOf(agent);
        return '<tr class="agent-row" data-agent="' + escapeHtml(agent.agent_id) + '">' +
          '<td><span class="tag ' + escapeHtml(agent.status) + '">' + escapeHtml(agentStatusLabel(agent.status)) + "</span></td>" +
          "<td><strong>" + escapeHtml(asset.hostname) + "</strong></td>" +
          "<td>" + formatSystem(asset) + "</td>" +
          "<td>" + escapeHtml(asset.arch) + "</td>" +
          "<td>" + escapeHtml(agent.agent_version) + "</td>" +
          "<td>" + escapeHtml(agent.desired_version) + "</td>" +
          '<td class="mono">' + escapeHtml(agent.agent_id) + "</td>" +
          "<td>" + escapeHtml(formatLocalTime(agent.last_online_at)) + "</td></tr>";
      }).join("") || '<tr><td colspan="8" class="muted">没有匹配的 Agent。</td></tr>';
      $$("[data-agent]", list).forEach(function (row) {
        row.addEventListener("click", function () {
          show(row.dataset.agent);
        });
      });
    }

    agents = (await request("/api/v1/agents")).agents;
    renderFilters();
    bindFilterEvents();
    close.addEventListener("click", closeDetail);
    backdrop.addEventListener("click", closeDetail);
    render();
  }

  async function initPackages() {
    const form = $("[data-package-upload-form]");
    const list = $("[data-package-list]");
    const filters = $("[data-package-filters]");
    const defaultsList = $("[data-default-release-list]");
    const uploadDialog = $("[data-package-upload-dialog]");
    const openUploadDialogButton = $("[data-open-upload-dialog]");
    const closeUploadDialogButton = $("[data-close-upload-dialog]");
    const detail = $("[data-package-detail]");
    const detailBody = $("[data-package-detail-body]");
    const detailClose = $("[data-package-close]");
    const detailBackdrop = $("[data-package-backdrop]");
    const defaultDialog = $("[data-default-release-dialog]");
    const defaultDialogTitle = $("[data-default-release-dialog-title]");
    const defaultDialogContext = $("[data-default-release-dialog-context]");
    const defaultReleaseOptions = $("[data-default-release-options]");
    const confirmDefaultRelease = $("[data-confirm-default-release]");
    const closeDefaultReleaseDialog = $("[data-close-default-release-dialog]");
    let packages = [];
    const state = {component: "", platform: "", arch: "", status: "", search: ""};
    let pendingDefaultTarget = null;

    function valuesFor(field) {
      return Array.from(new Set(packages.map(function (item) {
        return item[field];
      }).filter(Boolean))).sort();
    }

    function renderFilters() {
      renderFilterGroup($('[data-package-filter-group="component"]', filters), valuesFor("component"), state.component, componentLabel);
      renderFilterGroup($('[data-package-filter-group="platform"]', filters), valuesFor("platform"), state.platform, platformLabel);
      renderFilterGroup($('[data-package-filter-group="arch"]', filters), valuesFor("arch"), state.arch);
      renderFilterGroup($('[data-package-filter-group="status"]', filters), valuesFor("status"), state.status, packageStatusLabel);
    }

    function bindFilterEvents() {
      ["component", "platform", "arch", "status"].forEach(function (field) {
        const container = $('[data-package-filter-group="' + field + '"]', filters);
        container.addEventListener("click", function (event) {
          const button = event.target.closest("[data-value]");
          if (!button) return;
          state[field] = button.dataset.value || "";
          renderFilters();
          renderPackages();
        });
      });
      $('[name="search"]', filters).addEventListener("input", function (event) {
        state.search = event.target.value || "";
        renderPackages();
      });
      $("[data-reset-package-filters]").addEventListener("click", function () {
        state.component = "";
        state.platform = "";
        state.arch = "";
        state.status = "";
        state.search = "";
        $('[name="search"]', filters).value = "";
        renderFilters();
        renderPackages();
      });
    }

    function visiblePackages() {
      const needle = state.search.toLowerCase();
      return packages.filter(function (item) {
        return (!state.component || item.component === state.component) &&
          (!state.platform || item.platform === state.platform) &&
          (!state.arch || item.arch === state.arch) &&
          (!state.status || item.status === state.status) &&
          (!needle || [item.version, item.component, item.platform, item.arch].join(" ").toLowerCase().includes(needle));
      });
    }

    function groupPackages(items) {
      const groups = {};
      items.forEach(function (item) {
        const key = [item.component, item.platform, item.arch, item.build_type, item.version].join("|");
        if (!groups[key]) groups[key] = [];
        groups[key].push(item);
      });
      return Object.keys(groups).sort().map(function (key) {
        return groups[key];
      });
    }

    async function publishPackage(id, channel) {
      await request("/api/v1/admin/packages/" + id + "/publish", {
        method: "POST",
        headers: {"content-type": "application/json"},
        body: JSON.stringify({channel: channel})
      });
      setStatus("packages", "已发布到" + channelLabel(channel), true);
      await refresh();
    }

    async function retirePackage(id) {
      await request("/api/v1/admin/packages/" + id + "/retire", {method: "POST"});
      setStatus("packages", "安装包已退役", true);
      await refresh();
    }

    function defaultReleaseCombos() {
      const combos = {};
      packages.forEach(function (item) {
        const channels = ["stable", "candidate", "dev"];
        channels.forEach(function (channel) {
          if (channel === "stable" && item.build_type === "debug") {
            return;
          }
          const key = [item.component, item.platform, item.arch, item.build_type, channel].join("|");
          if (!combos[key]) {
            combos[key] = {
              component: item.component,
              platform: item.platform,
              arch: item.arch,
              build_type: item.build_type,
              channel: channel,
              current: null,
            };
          }
          if (Array.isArray(item.published_channels) && item.published_channels.indexOf(channel) >= 0) {
            combos[key].current = item;
          }
        });
      });
      return Object.keys(combos).sort().map(function (key) {
        return combos[key];
      });
    }

    function renderDefaultReleases() {
      const combos = defaultReleaseCombos();
      defaultsList.innerHTML = combos.map(function (combo) {
        const currentText = combo.current ?
          combo.current.version + " / " + buildTypeLabel(combo.current.build_type) :
          "暂无默认版本";
        return '<section class="default-release-row">' +
          '<div class="default-release-meta">' +
          '<strong>' + escapeHtml(componentLabel(combo.component)) + '</strong>' +
          '<span>' + escapeHtml(platformLabel(combo.platform)) + '</span>' +
          '<span>' + escapeHtml(combo.arch) + '</span>' +
          '<span>' + escapeHtml(buildTypeLabel(combo.build_type)) + '</span>' +
          '<span>' + escapeHtml(channelLabel(combo.channel)) + '</span>' +
          '<span>' + escapeHtml(currentText) + '</span>' +
          '</div>' +
          '<button class="button compact" type="button" data-adjust-default-release="' +
          escapeHtml([combo.component, combo.platform, combo.arch, combo.build_type, combo.channel].join("|")) + '">调整</button>' +
          '</section>';
      }).join("") || '<p class="muted">暂无默认版本配置。</p>';

      $$("[data-adjust-default-release]", defaultsList).forEach(function (button) {
        button.addEventListener("click", function () {
          const parts = button.dataset.adjustDefaultRelease.split("|");
          openDefaultReleaseDialog({
            component: parts[0],
            platform: parts[1],
            arch: parts[2],
            build_type: parts[3],
            channel: parts[4],
          });
        });
      });
    }

    function openDefaultReleaseDialog(target) {
      pendingDefaultTarget = target;
      const candidates = packages.filter(function (item) {
        return item.component === target.component &&
          item.platform === target.platform &&
          item.arch === target.arch &&
          item.build_type === target.build_type &&
          item.status !== "retired" &&
          !(target.channel === "stable" && item.build_type === "debug");
      });
      defaultDialogTitle.textContent = "调整默认版本";
      defaultDialogContext.textContent =
        componentLabel(target.component) + " / " +
        platformLabel(target.platform) + " / " +
        target.arch + " / " + buildTypeLabel(target.build_type) + " / " +
        channelLabel(target.channel);
      defaultReleaseOptions.innerHTML = candidates.map(function (item, index) {
        const checked = Array.isArray(item.published_channels) && item.published_channels.indexOf(target.channel) >= 0;
        return '<label class="default-release-option">' +
          '<input type="radio" name="default_release_package" value="' + escapeHtml(item.package_id) + '"' + (checked || (!candidates.some(function (candidate) {
            return Array.isArray(candidate.published_channels) && candidate.published_channels.indexOf(target.channel) >= 0;
          }) && index === 0) ? " checked" : "") + '>' +
          '<span>' + escapeHtml(item.version + " / " + buildTypeLabel(item.build_type) + " / " + packageStatusLabel(item.status)) + '</span>' +
          '</label>';
      }).join("") || '<p class="muted">没有可发布到该通道的版本。</p>';
      confirmDefaultRelease.disabled = candidates.length === 0;
      defaultDialog.showModal();
    }

    function closePackageDetail() {
      detail.setAttribute("aria-hidden", "true");
      detailBackdrop.hidden = true;
    }

    function openPackageDetail(packageId) {
      const item = packages.find(function (entry) { return entry.package_id === packageId; });
      if (!item) return;
      const stableAction = item.build_type === "debug" ? "" :
        '<button class="button compact" type="button" data-detail-publish="' + escapeHtml(item.package_id) + '" data-channel="stable">发布到稳定通道</button>';
      detailBody.innerHTML = "<h2>" + escapeHtml(componentLabel(item.component) + " " + item.version) + "</h2>" +
        '<p class="detail-id">' + escapeHtml(item.package_id) + "</p>" +
        '<div class="drawer-section"><h3>包信息</h3><dl>' +
        "<dt>平台</dt><dd>" + escapeHtml(platformLabel(item.platform)) + "</dd>" +
        "<dt>架构</dt><dd>" + escapeHtml(item.arch) + "</dd>" +
        "<dt>构建</dt><dd>" + escapeHtml(buildTypeLabel(item.build_type)) + "</dd>" +
        "<dt>状态</dt><dd>" + escapeHtml(packageStatusLabel(item.status)) + "</dd>" +
        "<dt>文件名</dt><dd>" + escapeHtml(item.filename) + "</dd>" +
        "<dt>大小</dt><dd>" + escapeHtml(String(item.size_bytes)) + " bytes</dd>" +
        "<dt>SHA256</dt><dd class=\"mono\">" + escapeHtml(item.sha256) + "</dd>" +
        "<dt>上传时间</dt><dd>" + escapeHtml(formatLocalTime(item.uploaded_at)) + "</dd>" +
        "<dt>校验时间</dt><dd>" + escapeHtml(formatLocalTime(item.validated_at)) + "</dd>" +
        "<dt>发布时间</dt><dd>" + escapeHtml(formatLocalTime(item.published_at)) + "</dd>" +
        "<dt>退役时间</dt><dd>" + escapeHtml(formatLocalTime(item.retired_at)) + "</dd></dl></div>" +
        '<div class="drawer-section"><h3>发布操作</h3><div class="detail-actions">' +
        stableAction +
        '<button class="button compact" type="button" data-detail-publish="' + escapeHtml(item.package_id) + '" data-channel="candidate">发布到候选通道</button>' +
        '<button class="button compact" type="button" data-detail-publish="' + escapeHtml(item.package_id) + '" data-channel="dev">发布到开发通道</button>' +
        '<button class="button compact button-danger-text" type="button" data-detail-retire="' + escapeHtml(item.package_id) + '">退役</button>' +
        "</div></div>";
      detail.setAttribute("aria-hidden", "false");
      detailBackdrop.hidden = false;
      $$("[data-detail-publish]", detailBody).forEach(function (button) {
        button.addEventListener("click", async function () {
          await publishPackage(button.dataset.detailPublish, button.dataset.channel);
          openPackageDetail(button.dataset.detailPublish);
        });
      });
      $$("[data-detail-retire]", detailBody).forEach(function (button) {
        button.addEventListener("click", async function () {
          if (window.confirm("确认退役这个安装包吗？")) {
            await retirePackage(button.dataset.detailRetire);
            closePackageDetail();
          }
        });
      });
    }

    function renderPackages() {
      const groups = groupPackages(visiblePackages());
      list.innerHTML = groups.map(function (group) {
        const base = group[0];
        const timeLabel = base.status === "retired" ? "退役时间" :
          base.status === "published" ? "发布时间" :
          base.status === "validated" ? "校验时间" : "上传时间";
        const timeValue = base.status === "retired" ? base.retired_at :
          base.status === "published" ? base.published_at :
          base.status === "validated" ? base.validated_at : base.uploaded_at;
        return '<section class="repo-row" data-package-row="' + escapeHtml(base.package_id) + '">' +
          '<div class="repo-main"><span class="repo-title">' + escapeHtml(componentLabel(base.component) + " " + base.version) + '</span>' +
          '<span class="repo-sub">' + escapeHtml(base.package_id) + "</span></div>" +
          '<div class="repo-sub">' + escapeHtml(platformLabel(base.platform) + " / " + base.arch + " / " + buildTypeLabel(base.build_type)) + "</div>" +
          '<div class="repo-status"><span class="tag">' + escapeHtml(packageStatusLabel(base.status)) + "</span></div>" +
          '<div class="repo-time">' + escapeHtml(timeLabel + " " + formatAbsoluteTime(timeValue)) + "</div></section>";
      }).join("") || '<p class="muted">没有匹配的安装包。</p>';
      $$("[data-package-row]", list).forEach(function (row) {
        row.addEventListener("click", function () {
          openPackageDetail(row.dataset.packageRow);
        });
      });
    }

    async function refresh() {
      packages = (await request("/api/v1/admin/packages")).packages;
      renderFilters();
      renderDefaultReleases();
      renderPackages();
    }

    form.addEventListener("submit", async function (event) {
      event.preventDefault();
      try {
        const file = new FormData(form).get("package");
        const query = new URLSearchParams({filename: file.name});
        await request("/api/v1/admin/packages?" + query.toString(), {method: "POST", body: file});
        setStatus("packages", "安装包已上传并通过校验", true);
        form.reset();
        uploadDialog.close();
        await refresh();
      } catch (error) {
        setStatus("packages", error.message, false);
      }
    });

    openUploadDialogButton.addEventListener("click", function () {
      uploadDialog.showModal();
    });
    closeUploadDialogButton.addEventListener("click", function () {
      uploadDialog.close();
    });
    detailClose.addEventListener("click", closePackageDetail);
    detailBackdrop.addEventListener("click", closePackageDetail);
    closeDefaultReleaseDialog.addEventListener("click", function () {
      defaultDialog.close();
    });
    confirmDefaultRelease.addEventListener("click", async function () {
      const selected = $('input[name="default_release_package"]:checked', defaultReleaseOptions);
      if (!selected || !pendingDefaultTarget) return;
      await publishPackage(selected.value, pendingDefaultTarget.channel);
      defaultDialog.close();
    });

    bindFilterEvents();
    await refresh();
  }

  const initializers = {install: initInstall, agents: initAgents, packages: initPackages};
  const initialize = initializers[document.body.dataset.page];
  if (initialize) {
    initialize().catch(function (error) {
      setStatus(document.body.dataset.page, error.message, false);
    });
  }
}());
