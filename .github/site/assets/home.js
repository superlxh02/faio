"use strict";
// 入场动画：元素进入视口时淡入上移，同一容器内按顺序错峰（--reveal-delay）。
// 仅在 JS 可用且未开启"减少动态"时给 <html> 加 reveal-js，无 JS 时内容照常可见。
const revealItems = [...document.querySelectorAll("[data-reveal]")];
if (
  revealItems.length &&
  "IntersectionObserver" in window &&
  !window.matchMedia("(prefers-reduced-motion: reduce)").matches
) {
  document.documentElement.classList.add("reveal-js");
  const groupSizes = new Map();
  revealItems.forEach((item) => {
    const parent = item.parentElement;
    const index = groupSizes.get(parent) ?? 0;
    groupSizes.set(parent, index + 1);
    item.style.setProperty("--reveal-delay", `${Math.min(index, 4) * 45}ms`);
  });
  // 提前触发：元素刚碰到视口下沿就开始，跟随滚动而不是等滚动停
  const revealObserver = new IntersectionObserver(
    (entries) => {
      entries.forEach((entry) => {
        if (!entry.isIntersecting) return;
        entry.target.classList.add("in-view");
        revealObserver.unobserve(entry.target);
      });
    },
    { rootMargin: "0px 0px 6% 0px", threshold: 0 },
  );
  revealItems.forEach((item) => revealObserver.observe(item));
}
const menu = document.querySelector(".menu-toggle");
const navigation = document.querySelector("#mobile-nav");
menu.addEventListener("click", () => {
  const expanded = menu.getAttribute("aria-expanded") !== "true";
  menu.setAttribute("aria-expanded", String(expanded));
  menu.setAttribute("aria-label", expanded ? "关闭导航" : "打开导航");
  navigation.hidden = !expanded;
});
navigation.addEventListener("click", (event) => {
  if (!event.target.closest("a")) return;
  menu.setAttribute("aria-expanded", "false");
  menu.setAttribute("aria-label", "打开导航");
  navigation.hidden = true;
});
const tabs = [...document.querySelectorAll("[data-code-tab]")];
function selectTab(tab) {
  tabs.forEach((item) => {
    const selected = item === tab;
    item.setAttribute("aria-selected", String(selected));
    item.tabIndex = selected ? 0 : -1;
    document.querySelector(`#panel-${item.dataset.codeTab}`).hidden = !selected;
  });
}
tabs.forEach((tab, index) => {
  tab.addEventListener("click", () => selectTab(tab));
  tab.addEventListener("keydown", (event) => {
    let target;
    if (event.key === "ArrowRight") target = tabs[(index + 1) % tabs.length];
    if (event.key === "ArrowLeft")
      target = tabs[(index + tabs.length - 1) % tabs.length];
    if (event.key === "Home") target = tabs[0];
    if (event.key === "End") target = tabs[tabs.length - 1];
    if (target) {
      event.preventDefault();
      selectTab(target);
      target.focus();
    }
  });
});
document
  .querySelector("#copy-command")
  .addEventListener("click", async (event) => {
    const button = event.currentTarget;
    const command = document.querySelector(".install-command code");
    try {
      await navigator.clipboard.writeText(command.textContent);
      button.textContent = "已复制";
      document.querySelector("#copy-status").textContent = "克隆命令已复制";
    } catch {
      const range = document.createRange();
      range.selectNodeContents(command);
      const selection = window.getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
      document.querySelector("#copy-status").textContent =
        "请手动复制选中的命令";
    }
  });

