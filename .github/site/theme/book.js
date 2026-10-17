/* Extend mdBook without replacing its navigation, search, or theme machinery. */
(() => {
  // mdBook exposes path_to_root in its template; derive the root for nested chapters.
  const root = typeof path_to_root === "string" ? path_to_root : "./";
  const nav = document.createElement("nav");
  nav.className = "faio-book-nav";
  nav.setAttribute("aria-label", "faio 导航");
  nav.innerHTML = `<a class="faio-book-brand" href="${root}../"><img src="${root}../assets/favicon.svg" alt="">faio</a><span class="nav-label">开发者文档</span><a href="${root}index.html">指南</a><a href="${root}../api/index.html">API</a><a href="${root}examples/index.html">例程</a><a href="https://github.com/superlxh02/faio">GitHub ↗</a>`;
  document.body.prepend(nav);
  const icon = document.querySelector(
    'link[rel="shortcut icon"], link[rel="icon"]',
  );
  if (icon) {
    icon.href = `${root}../assets/favicon.svg`;
    icon.type = "image/svg+xml";
  }

  const main = document.querySelector("main");
  if (!main) return;
  const headings = [...main.querySelectorAll("h2[id],h3[id]")];
  if (headings.length) {
    const toc = document.createElement("nav");
    toc.className = "faio-book-toc";
    toc.setAttribute("aria-label", "本页目录");
    const label = document.createElement("strong");
    label.textContent = "本页目录";
    toc.append(label);
    headings.forEach((heading) => {
      const link = document.createElement("a");
      link.href = "#" + heading.id;
      link.textContent = heading.textContent;
      if (heading.tagName === "H3") link.className = "level-3";
      toc.append(link);
    });
    document.body.append(toc);
    const observer = new IntersectionObserver(
      (entries) => {
        entries.forEach((entry) => {
          if (!entry.isIntersecting) return;
          toc
            .querySelectorAll("a")
            .forEach((link) =>
              link.classList.toggle(
                "current",
                link.getAttribute("href") === "#" + entry.target.id,
              ),
            );
        });
      },
      { rootMargin: "-120px 0px -65% 0px" },
    );
    headings.forEach((heading) => observer.observe(heading));
  }
  const footer = document.createElement("footer");
  footer.className = "faio-doc-footer";
  footer.innerHTML = `<span>faio · Fast Async I/O</span><a href="${root}../api/index.html">API 参考 →</a>`;
  main.append(footer);
  const top = document.createElement("button");
  top.className = "faio-backtop";
  top.textContent = "↑";
  top.setAttribute("aria-label", "返回顶部");
  top.addEventListener("click", () =>
    window.scrollTo({
      top: 0,
      behavior: window.matchMedia("(prefers-reduced-motion: reduce)").matches
        ? "instant"
        : "smooth",
    }),
  );
  document.body.append(top);

  // Render diagrams locally: no CDN request, and text remains readable on failure.
  const diagrams = [...main.querySelectorAll("code.language-mermaid")];
  if (!diagrams.length || !window.mermaid) return;
  const dark = ["navy", "coal", "ayu"].some((theme) =>
    document.documentElement.classList.contains(theme),
  );
  mermaid.initialize({
    startOnLoad: false,
    securityLevel: "strict",
    theme: dark ? "dark" : "neutral",
    fontFamily: "system-ui, sans-serif",
    suppressErrorRendering: true,
  });
  (async () => {
    for (const [index, block] of diagrams.entries()) {
      const wrapper = document.createElement("div");
      wrapper.className = "mermaid";
      try {
        const { svg, bindFunctions } = await mermaid.render(
          `faio-diagram-${index}`,
          block.textContent,
        );
        wrapper.innerHTML = svg;
        block.closest("pre").replaceWith(wrapper);
        if (bindFunctions) bindFunctions(wrapper);
      } catch (error) {
        const note = document.createElement("p");
        note.className = "mermaid-error";
        note.textContent = "此图未能渲染，下面保留 Mermaid 源码。";
        block.closest("pre").before(note);
        console.warn("Mermaid diagram failed:", error);
      }
    }
    main.dataset.diagramsReady = "true";
  })();
})();
