const {createMermaidTheme,readMermaidPalette,hasMermaidOverrides} = (() => {// @ts-check
/** @typedef {{background:string,surface:string,text:string,muted:string,line:string,accent:string,accentSoft:string,positive:string,positiveSoft:string,warning:string,warningSoft:string,danger:string,dangerSoft:string,fontFamily:string,dark:boolean}} MermaidPalette */

/** The canonical ecosystem configuration. Callers supply resolved DS token colors as hex. @param {MermaidPalette} p */
const createMermaidTheme = (p) => ({
  theme: /** @type {const} */ ('base'),
  look: /** @type {const} */ ('classic'),
  startOnLoad: false,
  securityLevel: /** @type {const} */ ('strict'),
  suppressErrorRendering: true,
  htmlLabels: false,
  maxTextSize: 30000,
  maxEdges: 300,
  secure: [
    'secure',
    'securityLevel',
    'startOnLoad',
    'theme',
    'themeVariables',
    'themeCSS',
    'look',
    'htmlLabels',
    'flowchart',
    'layout',
    'sequence',
    'state',
    'class',
    'er',
    'pie',
    'gantt',
    'gitGraph',
    'journey',
  ],
  fontFamily: p.fontFamily,
  flowchart: {
    htmlLabels: false,
    useMaxWidth: false,
    curve: /** @type {const} */ ('rounded'),
    nodeSpacing: 24,
    rankSpacing: 32,
    padding: 12,
    diagramPadding: 12,
    wrappingWidth: 180,
    minNodeWidth: 80,
  },
  sequence: {
    useMaxWidth: false,
    actorMargin: 32,
    actorFontSize: 13,
    noteFontSize: 13,
    messageFontSize: 13,
    width: 120,
    height: 40,
    boxMargin: 8,
    messageMargin: 28,
  },
  themeVariables: {
    darkMode: p.dark,
    background: p.background,
    fontFamily: p.fontFamily,
    fontSize: '13px',
    radius: 12,
    primaryColor: p.accentSoft,
    primaryTextColor: p.text,
    primaryBorderColor: p.accent,
    secondaryColor: p.accentSoft,
    secondaryTextColor: p.text,
    secondaryBorderColor: p.accent,
    tertiaryColor: p.background,
    tertiaryTextColor: p.text,
    tertiaryBorderColor: p.line,
    textColor: p.text,
    lineColor: p.line,
    mainBkg: p.accentSoft,
    nodeBkg: p.accentSoft,
    nodeBorder: p.accent,
    nodeTextColor: p.text,
    clusterBkg: p.surface,
    clusterBorder: p.line,
    titleColor: p.text,
    defaultLinkColor: p.line,
    edgeLabelBackground: p.surface,
    actorBkg: p.accentSoft,
    actorBorder: p.accent,
    actorTextColor: p.text,
    actorLineColor: p.line,
    signalColor: p.line,
    signalTextColor: p.text,
    labelBoxBkgColor: p.surface,
    labelBoxBorderColor: p.line,
    labelTextColor: p.text,
    loopTextColor: p.text,
    activationBkgColor: p.accentSoft,
    activationBorderColor: p.accent,
    sequenceNumberColor: p.text,
    noteBkgColor: p.warningSoft,
    noteBorderColor: p.warning,
    noteTextColor: p.text,
    attributeBackgroundColorOdd: p.surface,
    attributeBackgroundColorEven: p.background,
    classText: p.text,
    relationColor: p.line,
    relationLabelColor: p.text,
    labelColor: p.text,
    altBackground: p.background,
    compositeBackground: p.background,
    compositeBorder: p.line,
    cScale0: p.accentSoft,
    cScale1: p.positiveSoft,
    cScale2: p.warningSoft,
    cScale3: p.dangerSoft,
    cScaleLabel0: p.text,
    cScaleLabel1: p.text,
    cScaleLabel2: p.text,
    cScaleLabel3: p.text,
    pie1: p.accentSoft,
    pie2: p.positiveSoft,
    pie3: p.warningSoft,
    pie4: p.dangerSoft,
    pieStrokeColor: p.line,
    pieOuterStrokeColor: p.line,
    pieSectionTextColor: p.text,
    pieTitleTextColor: p.text,
    pieLegendTextColor: p.text,
    taskBkgColor: p.accentSoft,
    taskBorderColor: p.accent,
    taskTextColor: p.text,
    taskTextOutsideColor: p.text,
    activeTaskBkgColor: p.accentSoft,
    activeTaskBorderColor: p.accent,
    doneTaskBkgColor: p.positiveSoft,
    doneTaskBorderColor: p.positive,
    critBkgColor: p.dangerSoft,
    critBorderColor: p.danger,
    gridColor: p.line,
    todayLineColor: p.accent,
    git0: p.accent,
    git1: p.positive,
    git2: p.warning,
    git3: p.danger,
  },
  themeCSS: `
    text, tspan { font-family: ${p.fontFamily} !important; font-size: 13px !important; }
    text.actor { fill: ${p.text}; stroke: none; }
    .pieTitleText, .titleText { font-size: 16px !important; font-weight: 600; }
    .node rect, .node polygon, .node circle, .node path, .actor, .labelBox { stroke-width: 1.25px; }
    .node rect, rect.actor, rect.note { rx: 12px; ry: 12px; }
    .nodeLabel, .cluster-label { font-weight: 500; }
    .edgePath path, .flowchart-link, .messageLine0, .messageLine1 {
      stroke-width: 1.25px; stroke-linecap: round; stroke-linejoin: round;
    }
    .edgeLabel rect { fill: ${p.surface}; stroke: ${p.line}; stroke-width: .5px; rx: 10px; ry: 10px; opacity: 1; }
    .edgeLabel text { fill: ${p.text}; }
    .cluster rect { rx: 12px; ry: 12px; stroke-width: 1px; }
    .node.kv-accent rect, .node.kv-accent polygon, .node.kv-accent circle, .node.kv-accent path { fill: ${p.accentSoft}; stroke: ${p.accent}; }
    .node.kv-positive rect, .node.kv-positive polygon, .node.kv-positive circle, .node.kv-positive path { fill: ${p.positiveSoft}; stroke: ${p.positive}; }
    .node.kv-warning rect, .node.kv-warning polygon, .node.kv-warning circle, .node.kv-warning path { fill: ${p.warningSoft}; stroke: ${p.warning}; }
    .node.kv-danger rect, .node.kv-danger polygon, .node.kv-danger circle, .node.kv-danger path { fill: ${p.dangerSoft}; stroke: ${p.danger}; }
    .node.kv-decision rect, .node.kv-decision polygon, .node.kv-decision path { fill: ${p.warningSoft}; stroke: ${p.warning}; stroke-dasharray: 3 3; }
  `,
})

/** Resolve light-dark(), aliases and alpha colors through the browser, then flatten to hex. @param {HTMLElement} element */
const readMermaidPalette = (element) => {
  const probe = element.ownerDocument.createElement('span')
  probe.hidden = true
  element.append(probe)
  /** @param {string} token @param {number[]} [background] */
  const rgb = (token, background = [255, 255, 255]) => {
    probe.style.color = `var(${token})`
    const values = (getComputedStyle(probe).color.match(/[\d.]+/g) ?? []).map(Number)
    const alpha = values[3] ?? 1
    return background.map((base, index) => Math.round((values[index] ?? base) * alpha + base * (1 - alpha)))
  }
  const base = rgb('--kv-panel-bg')
  /** @param {string} token */
  const hex = (token) =>
    `#${rgb(token, base)
      .map((value) => value.toString(16).padStart(2, '0'))
      .join('')}`
  const palette = {
    background: hex('--kv-panel-bg'),
    surface: hex('--kv-panel-bg-subtle'),
    text: hex('--kv-text-main'),
    muted: hex('--kv-text-secondary'),
    line: hex('--kv-text-muted'),
    accent: hex('--kv-color-accent'),
    accentSoft: hex('--kv-color-accent-soft'),
    positive: hex('--kv-positive'),
    positiveSoft: hex('--kv-positive-bg'),
    warning: hex('--kv-warning'),
    warningSoft: hex('--kv-warning-bg'),
    danger: hex('--kv-danger'),
    dangerSoft: hex('--kv-danger-bg'),
    fontFamily: getComputedStyle(element).getPropertyValue('--kv-font-sans').trim(),
    dark: base[0] < 128,
  }
  probe.remove()
  return palette
}

/** Keep content from overriding the ecosystem presentation/security contract. @param {string} source */
const hasMermaidOverrides = (source) =>
  /^\s*---/u.test(source) || /%%\s*\{|^\s*(?:style|classDef|linkStyle|click)\s/imu.test(source)

return {createMermaidTheme,readMermaidPalette,hasMermaidOverrides};})();
const {attachMermaidDiagrams,configureMermaidLoader} = (() => {// @ts-check

/** @typedef {{initialize: (config: ReturnType<typeof createMermaidTheme>) => void, render: (id:string, source:string, container?:Element) => Promise<{svg:string}>}} MermaidEngine */
/** @type {(() => Promise<MermaidEngine>) | null} */
let loader = null
/** @type {Promise<MermaidEngine> | null} */
let engine = null
let sequence = 0
let queue = Promise.resolve()
const DIAGRAM_INSET = 8
/**
 * Mermaid sizes its view box in the off-screen host, and the drawing can land elsewhere in that
 * box (shifted, clipped on one side). Once shown, the box is fitted to what is drawn here.
 * @internal Not part of the public runtime entry.
 * @param {SVGSVGElement} diagram
 */
const fitDiagram = (diagram) => {
  const box = typeof diagram.getBBox === 'function' ? diagram.getBBox() : null
  if (!box?.width || !box.height) return
  const inset = DIAGRAM_INSET
  diagram.setAttribute(
    'viewBox',
    `${box.x - inset} ${box.y - inset} ${box.width + 2 * inset} ${box.height + 2 * inset}`,
  )
  // Retained Mermaid width/height would preserve the old intrinsic ratio and scale the text.
  diagram.removeAttribute('width')
  diagram.removeAttribute('height')
}

/** A host bundles/pins Mermaid; all presentation and rendering policy stays here. @param {() => Promise<MermaidEngine>} load */
const configureMermaidLoader = (load) => {
  loader = load
  engine = null
}

/** Enhance SSR sources. Hidden help drawers are deferred until the runtime opens them. @param {Document | HTMLElement} [root] */
const attachMermaidDiagrams = (root = document) => {
  let disposed = false
  const figures = [...root.querySelectorAll('[data-ui="mermaid"]')].filter(
    (figure) => figure instanceof HTMLElement && !figure.closest('dialog:not([open])'),
  )
  /** @type {WeakMap<Element, number>} */
  const revisions = new WeakMap()
  /** @param {Element} figure */
  const render = (figure) => {
    if (!(figure instanceof HTMLElement)) return
    const source = figure.querySelector('[data-ui="mermaid-source"]')
    const code = source?.querySelector('code')
    const canvas = figure.querySelector('[data-ui="mermaid-canvas"]')
    const status = figure.querySelector('[data-ui="mermaid-status"]')
    if (
      !(source instanceof HTMLDetailsElement) ||
      !(canvas instanceof HTMLElement) ||
      !(status instanceof HTMLElement)
    )
      return
    const revision = (revisions.get(figure) ?? 0) + 1
    revisions.set(figure, revision)
    const current = () => !disposed && figure.isConnected && revisions.get(figure) === revision
    const text = code?.textContent ?? ''
    figure.dataset.state = 'loading'
    status.textContent = figure.dataset.loadingLabel ?? ''
    status.hidden = false
    canvas.hidden = true
    // Mermaid has process-wide configuration. Serialize initialize + render, including distinct themes.
    queue = queue
      .catch(() => {})
      .then(async () => {
        if (!current()) return
        try {
          if (!loader || hasMermaidOverrides(text))
            throw new Error('No renderer or unsupported diagram configuration')
          engine ??= loader().catch((error) => {
            engine = null
            throw error
          })
          const mermaid = await engine
          if (!current()) return
          await document.fonts?.ready
          mermaid.initialize(createMermaidTheme(readMermaidPalette(figure)))
          const id = `kv-mermaid-${++sequence}`
          // An off-screen host isolates Mermaid's temporary SVG from the application body.
          const host = document.createElement('div')
          host.style.cssText = 'position:fixed;left:-10000px;top:0;visibility:hidden;'
          document.body.append(host)
          let svg
          try {
            svg = (await mermaid.render(id, text, host)).svg
          } finally {
            host.remove()
          }
          if (!current()) return
          // Only the strict Mermaid engine's sanitized SVG crosses this boundary; Markdown never does.
          canvas.innerHTML = svg
          const diagram = canvas.querySelector('svg')
          if (!diagram) throw new Error('Renderer returned no SVG')
          canvas.hidden = false
          fitDiagram(diagram)
          const width = diagram.viewBox.baseVal.width
          diagram.style.width = `${width}px`
          diagram.style.maxWidth = 'none'
          diagram.style.height = 'auto'
          diagram.setAttribute('role', 'img')
          if (!diagram.querySelector('title'))
            diagram.setAttribute('aria-label', canvas.getAttribute('aria-label') ?? '')
          source.open = false
          status.hidden = true
          figure.dataset.state = 'ready'
        } catch {
          if (!current()) return
          figure.dataset.state = 'error'
          status.textContent = figure.dataset.errorLabel ?? ''
          source.open = true
        }
      })
  }
  for (const figure of figures) render(figure)
  const owners = new Set([
    document.documentElement,
    ...figures.map(
      (figure) =>
        figure.closest('[data-theme]') ??
        figure.closest('[data-kv-design-system]') ??
        document.documentElement,
    ),
  ])
  const observer = new MutationObserver(() => {
    for (const figure of figures) render(figure)
  })
  for (const owner of owners)
    observer.observe(owner, { attributes: true, attributeFilter: ['data-theme', 'data-kv-theme'] })
  const media = window.matchMedia('(prefers-color-scheme: dark)')
  const refresh = () => {
    for (const figure of figures) render(figure)
  }
  media.addEventListener('change', refresh)
  return () => {
    disposed = true
    observer.disconnect()
    media.removeEventListener('change', refresh)
  }
}

return {attachMermaidDiagrams,configureMermaidLoader};})();
const attachHelpDrawers = (() => {// @ts-check

const TRIGGERS = '[data-ui="app-switcher-help"], button[name="screen-help"]'

/** Native dialog lifetime: modeless desktop guidance, modal fullscreen mobile.
 * @param {Document | HTMLElement} [root]
 * @param {{beforeOpen?:(trigger:HTMLElement)=>HTMLElement|undefined}} [options] */
const attachHelpDrawers = (root = document, options = {}) => {
  const drawers = () => [...root.querySelectorAll('[data-ui="help-drawer"]')]
  const media = window.matchMedia('(max-width: 767px)')
  /** @type {HTMLDialogElement | null} */
  let active = null
  /** @type {HTMLElement | null} */
  let invoker = null
  /** @type {HTMLElement | null} */
  let returnTo = null
  let disposeDiagrams = () => {}
  let scrollLocked = false
  let previousOverflow = ''
  /** @param {boolean} modal */
  const syncScroll = (modal) => {
    if (modal && !scrollLocked) {
      previousOverflow = document.documentElement.style.overflow
      document.documentElement.style.overflow = 'hidden'
      scrollLocked = true
    } else if (!modal && scrollLocked) {
      document.documentElement.style.overflow = previousOverflow
      scrollLocked = false
    }
  }
  /** @param {HTMLElement} trigger */
  const guideFor = (trigger) => {
    // A sheet, record page or peek with its own guide keeps it; one without falls back to the
    // screen's guide, which the content slot renders beside the page rather than inside it.
    const scopes = [
      trigger.closest('[data-ui="modal-sheet"], [data-ui="record-page"], [data-ui="peek"]'),
      trigger.closest('[data-ui="app-shell"]') ?? root,
    ]
    for (const scope of scopes) {
      if (!scope) continue
      const current = drawers().filter((candidate) => scope.contains(candidate))
      const guide =
        current.find((candidate) => candidate.id === trigger.getAttribute('aria-controls')) ?? current[0]
      if (guide) return guide
    }
    return undefined
  }
  for (const trigger of root.querySelectorAll(TRIGGERS)) {
    if (!(trigger instanceof HTMLElement)) continue
    const drawer = guideFor(trigger)
    if (drawer) {
      trigger.setAttribute('aria-controls', drawer.id)
      trigger.setAttribute('aria-expanded', 'false')
    }
  }
  const close = (restore = true) => {
    disposeDiagrams()
    if (active?.open) active.close()
    syncScroll(false)
    invoker?.setAttribute('aria-expanded', 'false')
    if (restore && returnTo?.isConnected) returnTo.focus()
    active = null
    invoker = null
    returnTo = null
  }
  /** @param {Event} event */
  const request = (event) => {
    if (event.defaultPrevented || !(event.target instanceof HTMLElement)) return
    const trigger = event.target
    // Content-slot navigation replaces screen-owned guides while the shell lives on.
    const drawer = guideFor(trigger)
    if (!(drawer instanceof HTMLDialogElement)) return
    trigger.setAttribute('aria-controls', drawer.id)
    event.preventDefault()
    if (active === drawer && drawer.open) {
      close()
      return
    }
    close(false)
    invoker = trigger
    returnTo = trigger
    const hostReturnTo = options.beforeOpen?.(trigger)
    if (hostReturnTo instanceof HTMLElement) returnTo = hostReturnTo
    // Close navigation through its existing close control, so its inert/scroll state is restored.
    const navigation = trigger.closest('[data-ui="app-navigation"][open]')
    if (media.matches && navigation instanceof HTMLDetailsElement) {
      const controls = navigation.querySelector('[data-ui="navigation-drawer"]')?.id
      const toggle = [...root.querySelectorAll('[data-ui="navigation-toggle"]')].find(
        (node) => node.getAttribute('aria-controls') === controls,
      )
      if (toggle instanceof HTMLElement) returnTo = toggle
      const control = navigation.querySelector('[data-ui="navigation-close"]')
      if (control instanceof HTMLElement) control.click()
    }
    active = drawer
    syncScroll(media.matches)
    if (media.matches) drawer.showModal()
    else drawer.show()
    trigger.setAttribute('aria-expanded', 'true')
    const button = drawer.querySelector('[data-ui="help-close"]')
    if (button instanceof HTMLElement) button.focus()
    disposeDiagrams = attachMermaidDiagrams(drawer)
  }
  /** @param {MouseEvent} event */
  const click = (event) => {
    const trigger =
      event.target instanceof Element
        ? event.target.closest('[data-ui="app-switcher-help"], button[name="screen-help"]')
        : null
    if (trigger instanceof HTMLElement && root.contains(trigger) && !event.defaultPrevented) {
      event.preventDefault()
      trigger.dispatchEvent(
        new CustomEvent('ket:screen-help-request', {
          bubbles: true,
          cancelable: true,
          detail: { trigger },
        }),
      )
    }
    if (
      event.target instanceof Element &&
      event.target.closest('[data-ui="help-close"]') &&
      active?.contains(event.target)
    )
      close()
  }
  // Pressing anywhere outside the guide dismisses it, like the shared popups. Pointerdown, not
  // click: a text selection dragged out of the guide stays open, and keys never dismiss it.
  // The pointer moves focus itself, so it does not return to `?`; `?` toggles on its own.
  /** @param {PointerEvent} event */
  const pointerdown = (event) => {
    if (!active?.open || event.button !== 0 || !(event.target instanceof Element)) return
    if (active.contains(event.target) || event.target.closest(TRIGGERS)) return
    close(false)
  }
  /** @param {KeyboardEvent} event */
  const key = (event) => {
    if (!active?.open || event.defaultPrevented) return
    if (event.key === 'Escape') {
      event.preventDefault()
      close()
    } else if (media.matches && event.key === 'Tab') {
      const items = [
        ...active.querySelectorAll('a[href], button, input, select, textarea, summary, [tabindex]'),
      ].filter(
        (node) =>
          node instanceof HTMLElement &&
          node.tabIndex >= 0 &&
          !node.matches(':disabled') &&
          !node.closest('[hidden], [inert], [aria-hidden="true"], details:not([open]) > :not(summary)') &&
          (typeof node.checkVisibility !== 'function' || node.checkVisibility({ visibilityProperty: true })),
      )
      const first = items[0]
      const last = items.at(-1)
      if (event.shiftKey && (document.activeElement === first || document.activeElement === active)) {
        event.preventDefault()
        if (last instanceof HTMLElement) last.focus()
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault()
        if (first instanceof HTMLElement) first.focus()
      }
    }
  }
  const resize = () => {
    if (!active?.open) return
    const focus = document.activeElement
    active.close()
    syncScroll(media.matches)
    if (media.matches) active.showModal()
    else active.show()
    if (focus instanceof HTMLElement && active.contains(focus)) focus.focus()
  }
  root.addEventListener('ket:screen-help-request', request)
  document.addEventListener('click', click)
  document.addEventListener('pointerdown', pointerdown)
  document.addEventListener('keydown', key)
  media.addEventListener('change', resize)
  return () => {
    root.removeEventListener('ket:screen-help-request', request)
    document.removeEventListener('click', click)
    document.removeEventListener('pointerdown', pointerdown)
    document.removeEventListener('keydown', key)
    media.removeEventListener('change', resize)
    close(false)
  }
}

return attachHelpDrawers;})();
const attachNavigationTools = (() => {// @ts-check
/** @typedef {{ hidden: boolean | 'until-found', open: boolean, name: string | null }} NavigationSnapshot */
/** Case- and accent-insensitive labels, including Vietnamese Đ. @param {string} value */
const normalizeMenuSearch = (value) =>
  value
    .normalize('NFD')
    .replace(/[\u0300-\u036f]/g, '')
    .replace(/[đĐ]/g, 'd')
    .toLowerCase()
    .trim()

/**
 * Filter only the host's already-permitted links; restore branch state after searching.
 * @param {HTMLElement} groups
 * @param {string} query
 * @param {Map<HTMLElement, NavigationSnapshot>} snapshots
 */
const filterNavigationMenu = (groups, query, snapshots) => {
  const term = normalizeMenuSearch(query)
  for (const [element, state] of snapshots) {
    if (!groups.contains(element)) {
      snapshots.delete(element)
      continue
    }
    element.hidden = state.hidden
    if (element instanceof HTMLDetailsElement) {
      element.open = state.open
      if (state.name === null) element.removeAttribute('name')
      else element.setAttribute('name', state.name)
    }
  }
  if (!term) {
    snapshots.clear()
    return 0
  }
  /** @param {HTMLElement} element */
  const remember = (element) => {
    if (!snapshots.has(element))
      snapshots.set(element, {
        hidden: element.hidden,
        open: element instanceof HTMLDetailsElement && Boolean(element.open),
        name: element.getAttribute('name'),
      })
  }
  const links = /** @type {HTMLElement[]} */ ([...groups.querySelectorAll('[data-ui="navigation-item"]')])
  let matches = 0
  for (const link of links) {
    remember(link)
    const labels = [
      link.querySelector('[data-ui="navigation-item-label"]')?.textContent ?? link.textContent ?? '',
    ]
    for (let parent = link.parentElement; parent && parent !== groups; parent = parent.parentElement) {
      if (parent.matches('[data-ui="navigation-branch"]'))
        labels.unshift(
          parent.querySelector(':scope > summary [data-ui="navigation-item-label"]')?.textContent ?? '',
        )
      if (parent.matches('[data-ui="navigation-group"]'))
        labels.unshift(parent.querySelector(':scope > [data-ui="navigation-group-label"]')?.textContent ?? '')
    }
    const visible = !snapshots.get(link)?.hidden && normalizeMenuSearch(labels.join(' ')).includes(term)
    link.hidden = !visible
    if (visible) matches++
  }
  for (const element of /** @type {NodeListOf<HTMLElement>} */ (
    groups.querySelectorAll('[data-ui="navigation-branch"], [data-ui="navigation-group"]')
  )) {
    remember(element)
    const visible = [...element.querySelectorAll('[data-ui="navigation-item"]')].some(
      (link) => !(/** @type {HTMLElement} */ (link).hidden),
    )
    element.hidden = (snapshots.get(element)?.hidden ?? false) || !visible
    if (element instanceof HTMLDetailsElement) {
      // Native exclusive accordions otherwise close other matching branches.
      element.removeAttribute('name')
      element.open = visible
    }
  }
  return matches
}

/**
 * Narrow a settings directory to the links, modules and sections that match.
 * A module whose name matches keeps all its links. Returns how many links show.
 * @param {HTMLElement} directory
 * @param {string} query
 */
const filterSettingsDirectory = (directory, query) => {
  const term = normalizeMenuSearch(query)
  let shown = 0
  for (const section of directory.querySelectorAll('[data-ui="settings-directory-section"]')) {
    if (!(section instanceof HTMLElement)) continue
    let sectionShown = 0
    for (const module of section.querySelectorAll('[data-ui="settings-directory-module"]')) {
      if (!(module instanceof HTMLElement)) continue
      const title = module.querySelector('[data-ui="settings-directory-module-title"]')?.textContent ?? ''
      const whole = !term || normalizeMenuSearch(title).includes(term)
      let moduleShown = 0
      for (const link of module.querySelectorAll('[data-ui="settings-directory-link"]')) {
        const item = link.closest('li')
        if (!(item instanceof HTMLElement)) continue
        item.hidden = !(whole || normalizeMenuSearch(link.textContent ?? '').includes(term))
        if (!item.hidden) moduleShown++
      }
      module.hidden = moduleShown === 0
      sectionShown += moduleShown
    }
    section.hidden = sectionShown === 0
    shown += sectionShown
  }
  const empty = directory.querySelector('[data-ui="settings-directory-empty"]')
  if (empty instanceof HTMLElement) empty.hidden = !term || shown > 0
  return shown
}

/** Persistent shell controls. Content navigation never owns this state. @param {ParentNode} [root] */
const attachNavigationTools = (root = document) => {
  /** @type {(() => void)[]} */
  const cleanups = []
  for (const input of root.querySelectorAll('[data-ui="settings-directory-search-input"]')) {
    const directory = input.closest('[data-ui="settings-directory"]')
    if (!(input instanceof HTMLInputElement) || !(directory instanceof HTMLElement)) continue
    const filter = () => filterSettingsDirectory(directory, input.value)
    /** @param {KeyboardEvent} event */
    const clear = (event) => {
      if (event.key !== 'Escape' || !input.value) return
      event.preventDefault()
      input.value = ''
      filter()
    }
    input.addEventListener('input', filter)
    input.addEventListener('keydown', clear)
    // A value restored by the browser after back navigation is applied at once.
    if (input.value) filter()
    cleanups.push(() => {
      input.removeEventListener('input', filter)
      input.removeEventListener('keydown', clear)
      filterSettingsDirectory(directory, '')
    })
  }
  for (const navigation of root.querySelectorAll('[data-ui="app-navigation"][data-has-tools="true"]')) {
    const shell = navigation.closest('[data-ui="app-shell"]')
    const sidebarNode = navigation.closest('[data-ui="app-sidebar"]')
    const sidebar = sidebarNode instanceof HTMLElement ? sidebarNode : null
    /** @param {string} selector */
    const find = (selector) => {
      const element = navigation.querySelector(selector)
      return element instanceof HTMLElement ? element : null
    }
    const drawer = navigation.querySelector('[data-ui="navigation-drawer"]')
    const panel = find('[data-ui="navigation-search"]')
    const input = navigation.querySelector('[data-ui="navigation-search-input"]')
    const search = find('[data-ui="navigation-search-toggle"]')
    const collapse = find('[data-ui="navigation-collapse"]')
    const empty = find('[data-ui="navigation-search-empty"]')
    const groups = find('[data-ui="navigation-groups"]')
    if (
      !(shell instanceof HTMLElement) ||
      !(input instanceof HTMLInputElement) ||
      !panel ||
      !search ||
      !groups
    )
      continue
    const expand = /** @type {HTMLElement | undefined} */ (
      [...shell.querySelectorAll('[data-ui="app-switcher-expand"]')].find(
        (button) => button.getAttribute('aria-controls') === drawer?.id,
      )
    )
    const media = window.matchMedia('(width < 48rem)')
    const previousInert = sidebar?.inert ?? false
    const snapshots = /** @type {Map<HTMLElement, NavigationSnapshot>} */ (new Map())
    const filter = () => {
      const count = filterNavigationMenu(groups, input.value, snapshots)
      if (empty) empty.hidden = !normalizeMenuSearch(input.value) || count > 0
    }
    const toggleSearch = () => {
      panel.hidden = !panel.hidden
      search.setAttribute('aria-expanded', String(!panel.hidden))
      if (panel.hidden) {
        input.value = ''
        filter()
        search.focus()
      } else input.focus()
    }
    /** @param {KeyboardEvent} event */
    const searchKey = (event) => {
      if (event.key === 'Escape') {
        event.preventDefault()
        toggleSearch()
      }
    }
    const syncCollapsed = () => {
      const collapsed = shell.dataset.sidebarCollapsed === 'true' && !media.matches
      if (sidebar) sidebar.inert = collapsed || previousInert
      expand?.setAttribute('aria-expanded', String(!collapsed))
      collapse?.setAttribute('aria-expanded', String(!collapsed))
    }
    /** @param {boolean} collapsed */
    const setCollapsed = (collapsed) => {
      if (media.matches) return
      shell.dataset.sidebarCollapsed = String(collapsed)
      syncCollapsed()
      ;(collapsed ? expand : collapse)?.focus()
    }
    const fold = () => setCollapsed(true)
    const unfold = () => setCollapsed(false)
    search.addEventListener('click', toggleSearch)
    input.addEventListener('input', filter)
    input.addEventListener('keydown', searchKey)
    collapse?.addEventListener('click', fold)
    expand?.addEventListener('click', unfold)
    media.addEventListener('change', syncCollapsed)
    // The menu slot may be reconciled after navigation or a permission/context change.
    const observer = new MutationObserver(() => {
      if (input.value) filter()
    })
    observer.observe(groups, { childList: true, subtree: true })
    syncCollapsed()
    if (input.value) filter()
    cleanups.push(() => {
      observer.disconnect()
      filterNavigationMenu(groups, '', snapshots)
      if (sidebar) sidebar.inert = previousInert
      search.removeEventListener('click', toggleSearch)
      input.removeEventListener('input', filter)
      input.removeEventListener('keydown', searchKey)
      collapse?.removeEventListener('click', fold)
      expand?.removeEventListener('click', unfold)
      media.removeEventListener('change', syncCollapsed)
    })
  }
  return () => {
    for (const cleanup of cleanups) cleanup()
  }
}

return attachNavigationTools;})();
const attachApplyOnInput = (() => {// A GET filter form rendered by `RecordForm({ applyOnInput: true })` or `FilterBar({ action })` has no submit step: a choice
// applies at once and typed text after a pause. Whether the submit reloads the page or an island redraws
// the form in place, the field the reader was typing in gets focus back with the caret at the end, so
// typing continues.

const FORM = 'form[data-ui="record-form"][data-apply="input"], form[data-ui="filter-bar"][data-apply="input"]'
const FOCUS_KEY = 'kv.design-system.apply-on-input.focus'
const TYPING_PAUSE = 400
const FOCUS_WINDOW = 10_000
const TEXT_TYPES = new Set(['', 'text', 'search', 'email', 'tel', 'url', 'number'])

/** @param {EventTarget | null} target */
const typedField = (target) =>
  !!target &&
  'tagName' in target &&
  ((target.tagName === 'INPUT' && TEXT_TYPES.has(/** @type {HTMLInputElement} */ (target).type ?? '')) ||
    target.tagName === 'TEXTAREA')

// Nested hosts may attach the runtime again; an event still submits once.
const handled = new WeakSet()

/**
 * @param {Document} doc
 * @param {{ form?: unknown, field?: unknown, at?: unknown } | null} saved
 */
const refocus = (doc, saved) => {
  if (!saved || typeof saved.field !== 'string' || !(Date.now() - Number(saved.at) < FOCUS_WINDOW))
    return false
  const field = doc.getElementById(saved.field)
  const form = field && 'form' in field ? /** @type {HTMLInputElement} */ (field).form : null
  if (!field || !form || form.id !== saved.form || !form.matches(FORM) || !typedField(field)) return false
  const input = /** @type {HTMLInputElement} */ (field)
  input.focus()
  const end = input.value.length
  if (input.type !== 'number') input.setSelectionRange?.(end, end)
  return true
}

/**
 * @param {Document} [doc]
 * @param {Pick<Storage, 'getItem' | 'setItem' | 'removeItem'> | null} [storage]
 * @returns {() => void}
 */
const attachApplyOnInput = (doc = document, storage = sessionStorageOrNull()) => {
  /** @type {ReturnType<typeof setTimeout> | undefined} */
  let typing
  /** @type {(() => void) | undefined} */
  let stopFollowing
  // An island that redraws the form in place replaces the focused field; focus follows it to the new
  // field unless the reader has moved on. A reloaded page restores it from storage instead.
  /** @param {Element} field @param {{ form: string, field: string, at: number }} saved */
  const follow = (field, saved) => {
    stopFollowing?.()
    if (typeof MutationObserver !== 'function') return
    const observer = new MutationObserver(() => {
      // While the typed field stays, it either keeps focus or the reader has left it.
      if (field.isConnected) {
        if (doc.activeElement !== field) stopFollowing?.()
        return
      }
      const active = doc.activeElement
      if (active && active !== doc.body) return stopFollowing?.()
      if (refocus(doc, saved)) {
        storage?.removeItem(FOCUS_KEY)
        stopFollowing?.()
      }
    })
    const expire = setTimeout(() => stopFollowing?.(), FOCUS_WINDOW)
    stopFollowing = () => {
      observer.disconnect()
      clearTimeout(expire)
      stopFollowing = undefined
    }
    observer.observe(doc, { childList: true, subtree: true })
  }
  /** @param {HTMLFormElement} form @param {Element | null} field */
  const submit = (form, field) => {
    clearTimeout(typing)
    typing = undefined
    if (field?.id && form.id && typedField(field)) {
      const saved = { form: form.id, field: field.id, at: Date.now() }
      try {
        storage?.setItem(FOCUS_KEY, JSON.stringify(saved))
      } catch {
        // Storage denial only loses restoration after a reload; the filter still applies.
      }
      follow(field, saved)
    }
    form.requestSubmit()
  }
  /** @param {Event} event */
  const onEvent = (event) => {
    if (handled.has(event)) return
    const field = /** @type {HTMLInputElement | HTMLSelectElement | HTMLTextAreaElement | null} */ (
      event.target
    )
    const form = field && 'form' in field ? field.form : null
    if (!form?.matches(FORM)) return
    handled.add(event)
    if (typedField(field)) {
      if (event.type !== 'input') return
      clearTimeout(typing)
      typing = setTimeout(() => submit(form, field), TYPING_PAUSE)
      return
    }
    if (event.type === 'change') submit(form, field)
  }
  /** @param {Event} event */
  const onSubmit = (event) => {
    if (
      event.target instanceof Object &&
      'matches' in event.target &&
      /** @type {Element} */ (event.target).matches(FORM)
    ) {
      clearTimeout(typing)
      typing = undefined
    }
  }

  try {
    const saved = JSON.parse(storage?.getItem(FOCUS_KEY) ?? 'null')
    storage?.removeItem(FOCUS_KEY)
    refocus(doc, saved)
  } catch {
    // An unreadable entry is dropped; nothing else depends on it.
  }

  doc.addEventListener('input', onEvent)
  doc.addEventListener('change', onEvent)
  doc.addEventListener('submit', onSubmit, true)
  return () => {
    clearTimeout(typing)
    stopFollowing?.()
    doc.removeEventListener('input', onEvent)
    doc.removeEventListener('change', onEvent)
    doc.removeEventListener('submit', onSubmit, true)
  }
}

function sessionStorageOrNull() {
  try {
    return globalThis.sessionStorage ?? null
  } catch {
    return null
  }
}

return attachApplyOnInput;})();
const attachNumberEntry = (() => {// A number field with a `locale` (NumberField or MoneyField) is typed and read the way its language
// writes numbers: "28.600" is twenty-eight thousand six hundred in Vietnamese. The browser's number
// input reads "." as a decimal point whatever the language and shows no grouping, so these fields are
// text inputs. This runtime regroups a value when the reader leaves the field and puts the plain
// decimal ("28600") into the form data, which is what the server reads.

const FIELD = 'input[data-number-locale]'
const CANONICAL = /^(-?)(\d+)(?:\.(\d+))?$/u

/** @type {Map<string, { group: string, decimal: string }>} */
const known = new Map()

/** The grouping and decimal marks of a language, from the platform's own number format. */
const separators = (/** @type {string} */ locale) => {
  const cached = known.get(locale)
  if (cached) return cached
  let parts = []
  try {
    parts = new Intl.NumberFormat(locale).formatToParts(1234567.5)
  } catch {
    parts = new Intl.NumberFormat('en').formatToParts(1234567.5)
  }
  const marks = {
    group: parts.find((part) => part.type === 'group')?.value ?? ',',
    decimal: parts.find((part) => part.type === 'decimal')?.value ?? '.',
  }
  known.set(locale, marks)
  return marks
}

const escapeRegex = (/** @type {string} */ text) => text.replace(/[.*+?^${}()|[\]\\]/gu, '\\$&')

/**
 * The plain decimal ("-1234.5") a reader meant, or null when the text is not a number in that
 * language. Grouping marks are accepted only in groups of three; a single mark that cannot be
 * grouping (Vietnamese "12.5") reads as the decimal point of the other habit. Empty text stays empty.
 * @param {string} text
 * @param {string} locale
 * @returns {string | null}
 */
const parseLocaleNumber = (text, locale) => {
  if (typeof text !== 'string') return null
  let value = text.replace(/[\s  ]/gu, '').replace(/^−/u, '-')
  if (value === '') return ''
  const negative = value.startsWith('-')
  if (negative || value.startsWith('+')) value = value.slice(1)
  const { group, decimal } = separators(locale)
  let whole = value
  let fraction = ''
  const decimalAt = value.indexOf(decimal)
  if (decimalAt >= 0) {
    if (value.lastIndexOf(decimal) !== decimalAt) return null
    whole = value.slice(0, decimalAt)
    fraction = value.slice(decimalAt + 1)
    if (!/^\d+$/u.test(fraction)) return null
  }
  if (!/^\d*$/u.test(whole)) {
    if (new RegExp(`^\\d{1,3}(?:${escapeRegex(group)}\\d{3})+$`, 'u').test(whole))
      whole = whole.split(group).join('')
    else {
      const at = whole.indexOf(group)
      const single = decimalAt < 0 && at > 0 && at === whole.lastIndexOf(group)
      if (!single || !/^\d+$/u.test(whole.slice(0, at)) || !/^\d+$/u.test(whole.slice(at + 1))) return null
      fraction = whole.slice(at + 1)
      whole = whole.slice(0, at)
    }
  }
  if (whole === '' && fraction === '') return null
  whole = whole.replace(/^0+(?=\d)/u, '') || '0'
  const zero = /^0+$/u.test(whole) && !/[1-9]/u.test(fraction)
  return `${negative && !zero ? '-' : ''}${whole}${fraction ? `.${fraction}` : ''}`
}

/**
 * A plain decimal written for a language ("1.800.000" in Vietnamese). Anything else, such as text a
 * reader typed that is not a number, is shown as it is.
 * @param {string | number | null | undefined} value
 * @param {string} locale
 * @returns {string}
 */
const formatLocaleNumber = (value, locale) => {
  const text = String(value ?? '').trim()
  const match = CANONICAL.exec(text)
  if (!match) return text
  const { group, decimal } = separators(locale)
  const whole = (match[2] ?? '').replace(/\B(?=(\d{3})+(?!\d))/gu, group)
  return `${match[1]}${whole}${match[3] ? `${decimal}${match[3]}` : ''}`
}

/**
 * What a control holds as data: the plain decimal of a localized number field (its text, or the given
 * text such as its default value), and the text unchanged for any other control or a typo.
 * @param {{ value?: string, getAttribute?: (name: string) => string | null }} control
 * @param {string} [text]
 * @returns {string}
 */
const numberControlValue = (control, text = control?.value ?? '') => {
  const locale = control?.getAttribute?.('data-number-locale')
  if (!locale) return text
  const parsed = parseLocaleNumber(text, locale)
  return parsed === null ? text : parsed
}

/**
 * Put each localized field's plain decimal in the form data, in place of the text it shows. A name the
 * form submits from anything else as well is left as it is.
 * @param {readonly { name: string, value: string, getAttribute(name: string): string | null }[]} controls
 * @param {{ getAll(name: string): unknown[], delete(name: string): void, append(name: string, value: string): void }} formData
 */
const normalizeNumberEntries = (controls, formData) => {
  /** @type {Map<string, typeof controls[number][]>} */
  const byName = new Map()
  for (const control of controls) byName.set(control.name, [...(byName.get(control.name) ?? []), control])
  for (const [name, list] of byName) {
    const values = formData.getAll(name)
    if (values.length !== list.length || values.some((value) => typeof value !== 'string')) continue
    formData.delete(name)
    list.forEach((control, index) => {
      formData.append(name, numberControlValue(control, /** @type {string} */ (values[index])))
    })
  }
}

const attached = new WeakSet()

/**
 * @param {Document} doc
 * @returns {() => void}
 */
const attachNumberEntry = (doc = document) => {
  if (attached.has(doc)) return () => {}
  attached.add(doc)
  /** @param {Event} event */
  const regroup = (event) => {
    const input = /** @type {HTMLInputElement | null} */ (event.target)
    if (!input?.matches?.(FIELD) || input.readOnly) return
    const locale = input.getAttribute('data-number-locale') ?? ''
    const parsed = parseLocaleNumber(input.value, locale)
    if (!parsed) return
    const shown = formatLocaleNumber(parsed, locale)
    if (shown !== input.value) input.value = shown
  }
  /** @param {Event} event */
  const submit = (event) => {
    const form = /** @type {HTMLFormElement | null} */ (event.target)
    const formData = /** @type {FormDataEvent} */ (event).formData
    if (!form?.elements || !formData) return
    const controls = [...form.elements].filter(
      (control) =>
        /** @type {HTMLInputElement} */ (control).matches?.(FIELD) &&
        /** @type {HTMLInputElement} */ (control).name &&
        !(/** @type {HTMLInputElement} */ (control).disabled),
    )
    if (controls.length) normalizeNumberEntries(/** @type {HTMLInputElement[]} */ (controls), formData)
  }
  doc.addEventListener('focusout', regroup, true)
  doc.addEventListener('formdata', submit, true)
  return () => {
    doc.removeEventListener('focusout', regroup, true)
    doc.removeEventListener('formdata', submit, true)
    attached.delete(doc)
  }
}

return attachNumberEntry;})();
// @ts-check

/** Progressive enhancement for primitives; server views stay render-pure.
 * @param {ParentNode} root
 */
const attachPrimitives = (root) => {
  /** @type {Map<HTMLElement, string | null>} */
  const descriptions = new Map()
  const mixedDefaults = new WeakSet()
  const sync = () => {
    for (const input of root.querySelectorAll('input[data-indeterminate="true"]')) {
      if (input instanceof HTMLInputElement) {
        input.indeterminate = true
        mixedDefaults.add(input)
      }
    }
    for (const image of root.querySelectorAll('[data-ui="avatar-image"]')) {
      if (image instanceof HTMLImageElement && image.complete && image.naturalWidth === 0) image.hidden = true
    }
    // Older JSX triggers remain supported; new callback triggers are associated in SSR.
    for (const tooltip of root.querySelectorAll('[data-ui="tooltip"]')) {
      const content = tooltip.querySelector('[data-ui="tooltip-content"]')
      const trigger = tooltip.querySelector(
        '[data-ui="tooltip-trigger"] :is(button, a[href], input, select, textarea, [tabindex])',
      )
      if (!(trigger instanceof HTMLElement) || !content?.id) continue
      if (!descriptions.has(trigger)) descriptions.set(trigger, trigger.getAttribute('aria-describedby'))
      const ids = new Set((trigger.getAttribute('aria-describedby') ?? '').split(' ').filter(Boolean))
      ids.add(content.id)
      trigger.setAttribute('aria-describedby', [...ids].join(' '))
    }
  }
  sync()
  const observer = new MutationObserver(sync)
  observer.observe(root, { childList: true, subtree: true })

  /** @param {Event} event */
  const onError = (event) => {
    const image = event.target
    if (image instanceof HTMLImageElement && image.matches('[data-ui="avatar-image"]')) image.hidden = true
  }
  /** @param {Event} event */
  const onLoad = (event) => {
    const image = event.target
    if (image instanceof HTMLImageElement && image.matches('[data-ui="avatar-image"]')) image.hidden = false
  }
  /** @param {Event} event */
  const onChange = (event) => {
    const input = event.target
    if (input instanceof HTMLInputElement && input.hasAttribute('data-indeterminate')) {
      input.setAttribute('data-indeterminate', 'false')
      input.indeterminate = false
      input.setAttribute('aria-checked', String(input.checked))
    }
  }
  /** @param {Event} event */
  const onClick = (event) => {
    if (!(event.target instanceof Element)) return
    const clear = event.target.closest('[data-ui="field-clear"]')
    if (!(clear instanceof HTMLButtonElement) || clear.disabled) return
    const input = clear.closest('[data-ui="field-input"]')?.querySelector('input')
    if (!(input instanceof HTMLInputElement) || input.disabled || input.readOnly) return
    input.value = ''
    input.dispatchEvent(new Event('input', { bubbles: true }))
    input.dispatchEvent(new Event('change', { bubbles: true }))
    input.focus()
  }
  /** @param {Event} event */
  const onEnter = (event) => {
    if (!(event.target instanceof Element)) return
    const tooltip = event.target.closest('[data-ui="tooltip"]')
    if (!tooltip) return
    const from = event instanceof FocusEvent || event instanceof MouseEvent ? event.relatedTarget : null
    if (!(from instanceof Node) || !tooltip.contains(from)) tooltip.removeAttribute('data-dismissed')
  }
  /** @param {Event} event */
  const onKeyDown = (event) => {
    if (!(event instanceof KeyboardEvent) || event.key !== 'Escape') return
    const open = [...root.querySelectorAll('[data-ui="tooltip"]:not([data-dismissed="true"])')].filter(
      (tooltip) => tooltip.matches(':hover, :focus-within'),
    )
    if (!open.length) return
    for (const tooltip of open) tooltip.setAttribute('data-dismissed', 'true')
    event.preventDefault()
    event.stopImmediatePropagation()
  }
  /** @param {Event} event */
  const onReset = (event) => {
    const form = event.target
    if (!(form instanceof HTMLFormElement)) return
    queueMicrotask(() => {
      if (event.defaultPrevented) return
      for (const input of form.elements) {
        if (!(input instanceof HTMLInputElement) || !mixedDefaults.has(input)) continue
        input.indeterminate = true
        input.setAttribute('data-indeterminate', 'true')
        input.setAttribute('aria-checked', 'mixed')
      }
    })
  }
  root.addEventListener('reset', onReset)
  root.addEventListener('error', onError, true)
  root.addEventListener('load', onLoad, true)
  root.addEventListener('change', onChange)
  root.addEventListener('click', onClick)
  root.addEventListener('mouseover', onEnter)
  root.addEventListener('focusin', onEnter)
  root.addEventListener('keydown', onKeyDown, true)
  return () => {
    observer.disconnect()
    root.removeEventListener('reset', onReset)
    root.removeEventListener('error', onError, true)
    root.removeEventListener('load', onLoad, true)
    root.removeEventListener('change', onChange)
    root.removeEventListener('click', onClick)
    root.removeEventListener('mouseover', onEnter)
    root.removeEventListener('focusin', onEnter)
    root.removeEventListener('keydown', onKeyDown, true)
    for (const [trigger, description] of descriptions) {
      if (description === null) trigger.removeAttribute('aria-describedby')
      else trigger.setAttribute('aria-describedby', description)
    }
  }
}

const attachDatePickers = (() => {
// @ts-check
// UTC is only a stable axis for civil-date arithmetic, never a timezone conversion.
/** @param {string} value */
const parseDate = (value) => {
  if (!/^\d{4}-\d{2}-\d{2}$/.test(value)) return null
  const [year, month, day] = value.split('-').map(Number)
  if (year < 1 || year > 9999) return null
  const date = new Date(0)
  date.setUTCFullYear(year, month - 1, day)
  date.setUTCHours(0, 0, 0, 0)
  return date.getUTCFullYear() === year && date.getUTCMonth() === month - 1 && date.getUTCDate() === day
    ? date
    : null
}
/** @param {Date} date */
const dateText = (date) =>
  [
    String(date.getUTCFullYear()).padStart(4, '0'),
    String(date.getUTCMonth() + 1).padStart(2, '0'),
    String(date.getUTCDate()).padStart(2, '0'),
  ].join('-')
/** @param {string} value @param {number} days */
const addDays = (value, days) => {
  const date = parseDate(value)
  if (!date) return ''
  date.setUTCDate(date.getUTCDate() + days)
  const result = dateText(date)
  return parseDate(result) ? result : ''
}
/** @param {string} value @param {number} offset */
const moveMonth = (value, offset) => {
  const date = parseDate(value)
  if (!date) return ''
  const day = date.getUTCDate()
  date.setUTCDate(1)
  date.setUTCMonth(date.getUTCMonth() + offset)
  const last = new Date(date)
  last.setUTCMonth(last.getUTCMonth() + 1, 0)
  date.setUTCDate(Math.min(day, last.getUTCDate()))
  const result = dateText(date)
  return parseDate(result) ? result : ''
}
/** @param {string} value */
const monthStart = (value) => `${value.slice(0, 7)}-01`
/** @param {string} value */
const monthEnd = (value) => {
  const date = parseDate(value)
  if (!date) return ''
  date.setUTCMonth(date.getUTCMonth() + 1, 0)
  return dateText(date)
}
const presetIds = ['today', 'yesterday', 'last7', 'thisMonth', 'lastMonth', 'last30', 'last90']
/** @param {string} id @param {string} today @returns {[string, string] | null} */
const presetRange = (id, today) => {
  if (!parseDate(today)) return null
  if (id === 'today') return [today, today]
  if (id === 'yesterday') {
    const day = addDays(today, -1)
    return day ? [day, day] : null
  }
  if (id === 'thisMonth') return [monthStart(today), monthEnd(today)]
  if (id === 'lastMonth') {
    const day = moveMonth(today, -1)
    return day ? [monthStart(day), monthEnd(day)] : null
  }
  const days = { last7: 7, last30: 30, last90: 90 }[/** @type {'last7'|'last30'|'last90'} */ (id)]
  const start = days ? addDays(today, 1 - days) : ''
  return start ? [start, today] : null
}
/** @param {string} value @param {string} [min] @param {string} [max] */
const withinBounds = (value, min = '', max = '') =>
  !!parseDate(value) && (!parseDate(min) || value >= min) && (!parseDate(max) || value <= max)
/** @param {string} start @param {string} end @returns {[string, string]} */
const orderedRange = (start, end) => (start <= end ? [start, end] : [end, start])

/** The order a language writes a day in, the way the person reading chose it, not the browser's:
 * English month first, Japanese year first, Vietnamese day first.
 * @param {string} [locale] @returns {('day' | 'month' | 'year')[]} */
const dayOrder = (locale) =>
  locale === 'en'
    ? ['month', 'day', 'year']
    : locale === 'ja'
      ? ['year', 'month', 'day']
      : ['day', 'month', 'year']
/** The pattern shown in an empty date field. @param {string} [locale] */
const dayPattern = (locale) => {
  const pattern = dayOrder(locale)
    .map((part) => ({ day: 'dd', month: 'mm', year: 'yyyy' })[part])
    .join('/')
  return locale === 'vi' || !locale ? pattern : pattern.toUpperCase()
}
/** A civil date written in the reader's order; '' for anything that is not one.
 * @param {string} value @param {string} [locale] @param {boolean} [year] */
const formatDay = (value, locale, year = true) => {
  if (!parseDate(value)) return ''
  const parts = { year: value.slice(0, 4), month: value.slice(5, 7), day: value.slice(8, 10) }
  return dayOrder(locale)
    .filter((part) => year || part !== 'year')
    .map((part) => parts[part])
    .join('/')
}
/** Reads a date typed in the reader's order (any of / . - or spaces between, one or two digits for the
 * day and month) or as an ISO date; '' when it is not a real calendar day.
 * @param {string} text @param {string} [locale] */
const parseDay = (text, locale) => {
  const value = text.trim()
  if (parseDate(value)) return value
  const numbers = value.split(/[\s./-]+/u)
  if (numbers.length !== 3 || numbers.some((number) => !/^\d+$/u.test(number))) return ''
  /** @type {Record<string, string>} */
  const parts = {}
  dayOrder(locale).forEach((part, index) => {
    parts[part] = numbers[index]
  })
  if (parts.year.length !== 4 || parts.month.length > 2 || parts.day.length > 2) return ''
  const iso = `${parts.year}-${parts.month.padStart(2, '0')}-${parts.day.padStart(2, '0')}`
  return parseDate(iso) ? iso : ''
}
/** Compact display keeps the shared year once, in the reader's order; submitted values remain ISO civil dates.
 * @param {string} start @param {string} end @param {string} [locale] */
const formatRange = (start, end, locale = 'vi') => {
  if (!parseDate(start) || !parseDate(end)) return ''
  return `${formatDay(start, locale, start.slice(0, 4) !== end.slice(0, 4))} – ${formatDay(end, locale)}`
}

// @ts-check

/** An already validated calendar day. @param {string} value */
const civil = (value) => {
  const date = parseDate(value)
  if (!date) throw new RangeError(`Invalid civil date: ${value}`)
  return date
}
/** @param {HTMLInputElement} input @param {string} value */
const allowed = (input, value) => {
  if (!withinBounds(value, input.min, input.max)) return false
  const step = Number(input.step || 1)
  if (input.step === 'any' || !Number.isFinite(step) || step <= 0) return true
  const base = parseDate(input.min) ?? parseDate(input.getAttribute('value') ?? '') ?? civil('1970-01-01')
  return Math.abs(((civil(value).getTime() - base.getTime()) / 86400000 / step) % 1) < 0.000001
}
/** @type {WeakMap<HTMLElement, { users: number, prior: boolean }>} */
const inertLayers = new WeakMap()
let scrollLocks = 0
let priorOverflow = ''
let priorOverflowPriority = ''
/** Make the fullscreen calendar modal while preserving any outer dialog's locks.
 * @param {HTMLElement} popup */
const lockBackground = (popup) => {
  /** @type {HTMLElement[]} */
  const siblings = []
  let branch = popup
  while (branch.parentElement) {
    const parent = branch.parentElement
    for (const sibling of parent.children) {
      if (!(sibling instanceof HTMLElement) || sibling === branch) continue
      const state = inertLayers.get(sibling) ?? { users: 0, prior: sibling.inert }
      state.users++
      inertLayers.set(sibling, state)
      sibling.inert = true
      siblings.push(sibling)
    }
    if (parent === document.body) break
    branch = parent
  }
  const style = document.documentElement.style
  if (scrollLocks++ === 0) {
    priorOverflow = style.overflow
    priorOverflowPriority = style.getPropertyPriority('overflow')
    style.setProperty('overflow', 'hidden')
  }
  return () => {
    for (const sibling of siblings) {
      const state = inertLayers.get(sibling)
      if (state && --state.users === 0) {
        sibling.inert = state.prior
        inertLayers.delete(sibling)
      }
    }
    if (--scrollLocks === 0) {
      if (priorOverflow) style.setProperty('overflow', priorOverflow, priorOverflowPriority)
      else style.removeProperty('overflow')
    }
  }
}
/** Progressive compound date/time: one canonical submitted wall-time value.
 * @param {HTMLElement} picker */
const enhanceDateTime = (picker) => {
  const input = picker.querySelector('input[type="datetime-local"]')
  // The date part's own picker may already have turned its native field into a hidden one.
  const day = picker.querySelector('[data-ui="date-control-row"] > input')
  const time = picker.querySelector('input[type="time"]')
  const parts = picker.querySelector('[data-ui="date-time-parts"]')
  if (
    !(input instanceof HTMLInputElement) ||
    !(day instanceof HTMLInputElement) ||
    !(time instanceof HTMLInputElement) ||
    !(parts instanceof HTMLElement)
  )
    return () => {}
  const validator = /** @type {HTMLInputElement} */ (input.cloneNode())
  const label = picker.querySelector('label')
  const originalFor = label?.getAttribute('for')
  const innerLabel = parts.querySelector('label')
  const innerFor = innerLabel?.getAttribute('for')
  const form = input.form
  input.type = 'hidden'
  parts.hidden = false
  const entry = parts.querySelector('[data-ui="date-entry"] input')
  label?.setAttribute('for', entry && !entry.closest('[hidden]') ? entry.id : day.id)
  innerLabel?.removeAttribute('for')
  const syncControls = () => {
    validator.disabled = input.disabled
    validator.readOnly = input.readOnly
    for (const node of parts.querySelectorAll('input,button')) {
      if (node instanceof HTMLInputElement || node instanceof HTMLButtonElement)
        node.disabled = input.disabled || (node instanceof HTMLButtonElement && input.readOnly)
      if (node instanceof HTMLInputElement) node.readOnly = input.readOnly
    }
  }
  syncControls()
  // An island can lock a saved record without replacing this compound field.
  const controlsObserver = typeof MutationObserver === 'undefined' ? null : new MutationObserver(syncControls)
  controlsObserver?.observe(input, { attributes: true, attributeFilter: ['disabled', 'readonly'] })
  for (const part of [day, time, ...(entry ? [entry] : [])]) {
    const description = input.getAttribute('aria-describedby')
    if (description) part.setAttribute('aria-describedby', description)
  }
  const sync = () => {
    // Partial drafts survive record view-state re-renders; native child validation
    // prevents submission until both pieces are present.
    const value = day.value || time.value ? `${day.value}T${time.value}` : ''
    validator.value = day.value && time.value ? value : ''
    const partial = Boolean(day.value) !== Boolean(time.value)
    time.setCustomValidity(
      partial
        ? picker.dataset.dateLocale === 'en'
          ? 'Choose date and time.'
          : 'Chọn đủ ngày và giờ.'
        : validator.validationMessage,
    )
    if (input.value === value) return
    input.value = value
    input.dispatchEvent(new Event('input', { bubbles: true }))
    input.dispatchEvent(new Event('change', { bubbles: true }))
  }
  /** @param {Event} event */
  const change = (event) => {
    if (event.target === day || event.target === time) sync()
  }
  let active = true
  const reset = () =>
    queueMicrotask(() => {
      if (!active) return
      const [date = '', clock = ''] = input.value.split('T')
      day.value = date
      time.value = clock
      sync()
    })
  picker.addEventListener('input', change)
  picker.addEventListener('change', change)
  form?.addEventListener('reset', reset)
  sync()
  return () => {
    active = false
    controlsObserver?.disconnect()
    picker.removeEventListener('input', change)
    picker.removeEventListener('change', change)
    form?.removeEventListener('reset', reset)
    input.type = 'datetime-local'
    parts.hidden = true
    if (originalFor) label?.setAttribute('for', originalFor)
    if (innerFor) innerLabel?.setAttribute('for', innerFor)
    for (const node of parts.querySelectorAll('input,button'))
      if (node instanceof HTMLInputElement || node instanceof HTMLButtonElement) node.disabled = true
  }
}
/** @param {HTMLElement} picker */
const enhance = (picker) => {
  if (picker.dataset.dateMode === 'datetime') return enhanceDateTime(picker)
  const panel = picker.querySelector('[data-ui="date-calendar"]')
  const trigger = picker.querySelector('[name="kv-date-open"]')
  const inputs = /** @type {HTMLInputElement[]} */ ([
    ...picker.querySelectorAll('[data-ui="date-inputs"] input[type="date"], [data-ui="date-boundary"]'),
  ])
  if (
    !(panel instanceof HTMLElement) ||
    !(trigger instanceof HTMLButtonElement) ||
    !inputs.length ||
    !('showPopover' in panel)
  )
    return () => {}
  const popup = panel
  const opener = trigger
  const range = picker.dataset.dateMode === 'range'
  const locale =
    picker.dataset.dateLocale === 'ja' ? 'ja-JP' : picker.dataset.dateLocale === 'en' ? 'en-US' : 'vi-VN'
  const weekStart = Number(picker.dataset.dateWeekStart ?? 1)
  const labels = /** @type {import('./index.tsx').DatePickerLabels} */ (
    JSON.parse(picker.dataset.dateLabels ?? '{}')
  )
  const months = /** @type {HTMLElement} */ (popup.querySelector('[data-ui="date-calendar-months"]'))
  const status = /** @type {HTMLElement} */ (popup.querySelector('[data-ui="date-calendar-status"]'))
  const apply = /** @type {HTMLButtonElement} */ (popup.querySelector('[value="apply"]'))
  const message = picker.querySelector('[data-ui="date-range-error"]')
  const end = inputs[1]
  const defaults = inputs.map((input) => input.value)
  const display = range ? picker.querySelector('input[type="text"]') : null
  const preset = picker.querySelector('select')
  const field = display instanceof HTMLInputElement ? display : end
  const originalInvalid = field?.getAttribute('aria-invalid')
  const originalDescription = field?.getAttribute('aria-describedby')
  let returnFocus = /** @type {HTMLElement} */ (opener)
  // A native date field writes the day in the browser's language. A single date is typed and shown in
  // the reader's instead; the native field, made hidden, still carries the submitted ISO value.
  const native = inputs[0]
  const entryBox = range ? null : picker.querySelector('[data-ui="date-entry"]')
  const entryInput = entryBox?.querySelector('input')
  const entry = entryBox instanceof HTMLElement && entryInput instanceof HTMLInputElement ? entryInput : null
  const dayLocale = picker.dataset.dateLocale
  const validator = entry ? /** @type {HTMLInputElement} */ (native.cloneNode()) : null
  const invalidDay =
    dayLocale === 'en'
      ? `Enter a date as ${dayPattern('en')}.`
      : dayLocale === 'ja'
        ? `${dayPattern('ja')} の形式で入力してください。`
        : `Nhập ngày theo dạng ${dayPattern('vi')}.`
  const checkEntry = () => {
    if (!entry || !validator) return
    validator.required = native.required
    validator.value = native.value
    entry.setCustomValidity(entry.value.trim() && !native.value ? invalidDay : validator.validationMessage)
  }
  const showEntry = () => {
    if (!entry) return
    entry.value = formatDay(native.value, dayLocale)
    checkEntry()
  }
  const syncEntryControls = () => {
    if (!entry) return
    entry.disabled = native.disabled
    entry.readOnly = native.readOnly
  }
  /** @param {Event} event */
  const typing = (event) => {
    if (!entry || event.target !== entry) return
    const value = parseDay(entry.value, dayLocale)
    if (event.type === 'change' && value) entry.value = formatDay(value, dayLocale)
    if (native.value !== value) {
      native.value = value
      native.dispatchEvent(new Event('input', { bubbles: true }))
      native.dispatchEvent(new Event('change', { bubbles: true }))
    }
    checkEntry()
  }
  /** @type {[HTMLLabelElement, string][]} */
  const labelled = []
  const readToday = () => {
    const local = new Date()
    const fallback = `${local.getFullYear()}-${String(local.getMonth() + 1).padStart(2, '0')}-${String(local.getDate()).padStart(2, '0')}`
    return parseDate(picker.dataset.dateToday ?? '') ? (picker.dataset.dateToday ?? fallback) : fallback
  }
  const formatter = new Intl.DateTimeFormat(locale, {
    day: 'numeric',
    month: 'long',
    year: 'numeric',
    timeZone: 'UTC',
  })
  const short = new Intl.DateTimeFormat(locale, {
    day: '2-digit',
    month: '2-digit',
    year: 'numeric',
    timeZone: 'UTC',
  })
  const monthFormat = new Intl.DateTimeFormat(locale, { month: 'long', year: 'numeric', timeZone: 'UTC' })
  const weekdayFormat = new Intl.DateTimeFormat(locale, { weekday: 'short', timeZone: 'UTC' })
  let today = readToday()
  let month = ''
  let focusDate = ''
  let preferredPreset = ''
  /** @type {string[]} */
  let draft = []
  const isOpen = () => popup.matches(':popover-open')
  const isMobile = () => window.innerWidth < 768
  const submitFilter = () => {
    const form = inputs[0].form
    // A CSR editor often intercepts submit without a method attribute. Only an
    // explicitly declared GET filter opts into automatic calendar submission.
    if (form?.getAttribute('method')?.toLowerCase() === 'get') form.requestSubmit()
  }
  /** @type {(() => void) | null} */
  let unlock = null
  const syncPresentation = () => {
    if (isOpen() && isMobile()) {
      unlock ??= lockBackground(popup)
      popup.setAttribute('aria-modal', 'true')
    } else {
      unlock?.()
      unlock = null
      popup.removeAttribute('aria-modal')
    }
  }
  const monthCount = () => (range ? (isMobile() ? 12 : 2) : 1)
  /** @param {string} value */
  const calendarStart = (value) => {
    const start = monthStart(value)
    return range && isMobile() ? moveMonth(start, -2) || start : start
  }
  const visibleMonth = () => {
    const top = months.getBoundingClientRect().top
    const section = [...months.children].find((child) => child.getBoundingClientRect().bottom > top + 1)
    return section instanceof HTMLElement ? (section.dataset.dateMonth ?? month) : month
  }
  const revealMonth = () => {
    if (!range || !isMobile()) return
    const section = months.querySelector(`[data-date-month="${monthStart(focusDate)}"]`)
    if (section instanceof HTMLElement) {
      months.scrollTop +=
        section.getBoundingClientRect().top -
        months.getBoundingClientRect().top -
        parseFloat(getComputedStyle(months).paddingTop)
    }
  }
  /** @param {HTMLElement} day */
  const revealDay = (day) => {
    if (!isMobile()) return
    const box = day.getBoundingClientRect()
    const viewport = months.getBoundingClientRect()
    if (box.top < viewport.top) months.scrollTop += box.top - viewport.top
    else if (box.bottom > viewport.bottom) months.scrollTop += box.bottom - viewport.bottom
  }
  const enabled = () => inputs.every((input) => !input.disabled && !input.readOnly)
  const valid = () =>
    !!draft[0] &&
    allowed(inputs[0], draft[0]) &&
    (!range || (!!draft[1] && draft[0] <= draft[1] && allowed(end, draft[1])))
  const validate = () => {
    if (!end || !field) return true
    const startValue = inputs[0].value
    const endValue = end.value
    const empty = !startValue && !endValue
    const invalid =
      !inputs[0].disabled &&
      !end.disabled &&
      (empty
        ? inputs.some((input) => input.required)
        : !allowed(inputs[0], startValue) || !allowed(end, endValue) || startValue > endValue)
    if (display instanceof HTMLInputElement)
      display.value = formatRange(startValue, endValue, picker.dataset.dateLocale)
    if (message instanceof HTMLElement) {
      message.hidden = !invalid
      field.setAttribute(
        'aria-describedby',
        [originalDescription, invalid ? message.id : ''].filter(Boolean).join(' '),
      )
    }
    if (invalid) field.setAttribute('aria-invalid', 'true')
    else if (originalInvalid) field.setAttribute('aria-invalid', originalInvalid)
    else field.removeAttribute('aria-invalid')
    if (preset instanceof HTMLSelectElement) {
      preset.disabled = !enabled()
      const matching = [...preset.options].filter((option) => {
        const period = presetRange(option.value, today)
        option.disabled = !!period && (!allowed(inputs[0], period[0]) || !allowed(end, period[1]))
        return period && period[0] === startValue && period[1] === endValue
      })
      preset.value =
        (matching.find((option) => option.value === preferredPreset) ?? matching[0])?.value ?? 'custom'
    }
    return !invalid
  }
  /** @param {string[]} values */
  const commit = (values) => {
    const changed = inputs.filter((input, index) => input.value !== values[index])
    inputs.forEach((input, index) => {
      input.value = values[index]
    })
    showEntry()
    validate()
    for (const input of [
      ...changed,
      ...(changed.length && display instanceof HTMLInputElement ? [display] : []),
    ]) {
      input.dispatchEvent(new Event('input', { bubbles: true }))
      input.dispatchEvent(new Event('change', { bubbles: true }))
    }
  }
  /** @param {string} [preview] */
  const paint = (preview = '') => {
    const [from, to] =
      range && draft[0] && !draft[1] && preview
        ? orderedRange(draft[0], preview)
        : [draft[0], draft[1] ?? draft[0]]
    for (const node of months.querySelectorAll('button[data-date]')) {
      const button = /** @type {HTMLButtonElement} */ (node)
      const day = button.dataset.date ?? ''
      const cell = button.parentElement
      const selected = day === draft[0] || day === draft[1]
      button.dataset.selected = String(selected)
      if (cell) {
        cell.dataset.inRange = String(range && !!from && !!to && day >= from && day <= to)
        cell.setAttribute(
          'aria-selected',
          String(selected || (range && !!from && !!to && day >= from && day <= to)),
        )
      }
    }
  }
  const position = () => {
    if (!isOpen() || isMobile()) return
    const anchor = picker.querySelector('[data-ui="date-field-control"]') ?? opener
    const rect = anchor.getBoundingClientRect()
    const inset = parseFloat(getComputedStyle(popup).scrollMarginTop)
    const gap = inset
    popup.style.setProperty('--kv-date-max-height', `${Math.max(0, window.innerHeight - inset * 2)}px`)
    const box = popup.getBoundingClientRect()
    const left = Math.max(
      inset,
      Math.min(
        rect.left + box.width <= window.innerWidth - inset ? rect.left : rect.right - box.width,
        window.innerWidth - box.width - inset,
      ),
    )
    const below = rect.bottom + gap
    const top =
      below + box.height <= window.innerHeight - inset ? below : Math.max(inset, rect.top - box.height - gap)
    popup.style.setProperty('--kv-date-left', `${left}px`)
    popup.style.setProperty('--kv-date-top', `${top}px`)
  }
  /** @param {boolean} [focus] */
  const render = (focus = false) => {
    const scrollTop = months.scrollTop
    months.replaceChildren()
    popup.dataset.months = String(monthCount())
    for (let index = 0; index < monthCount(); index++) {
      const current = moveMonth(month, index)
      if (!current) continue
      const section = document.createElement('section')
      section.dataset.ui = 'date-month'
      section.dataset.dateMonth = current
      const title = document.createElement('h4')
      title.dataset.ui = 'date-month-title'
      title.id = `${popup.id}-month-${index}`
      title.textContent = monthFormat.format(civil(current))
      if (!isMobile()) title.setAttribute('aria-live', 'polite')
      section.append(title)
      const table = document.createElement('table')
      table.dataset.ui = 'date-grid'
      table.setAttribute('role', 'grid')
      table.setAttribute('aria-labelledby', title.id)
      if (range) table.setAttribute('aria-multiselectable', 'true')
      const head = table.createTHead().insertRow()
      for (let column = 0; column < 7; column++) {
        const th = document.createElement('th')
        th.dataset.ui = 'date-weekday'
        th.scope = 'col'
        th.textContent = weekdayFormat.format(civil(addDays('2026-09-06', (column + weekStart) % 7)))
        head.append(th)
      }
      const body = table.createTBody()
      const offset = (civil(current).getUTCDay() - weekStart + 7) % 7
      const days = civil(monthEnd(current)).getUTCDate()
      const weeks = isMobile() ? Math.ceil((offset + days) / 7) : 6
      for (let row = 0; row < weeks; row++) {
        const tr = body.insertRow()
        for (let column = 0; column < 7; column++) {
          const cell = tr.insertCell()
          cell.dataset.ui = 'date-cell'
          const number = row * 7 + column - offset + 1
          if (number < 1 || number > days) {
            cell.setAttribute('aria-hidden', 'true')
            continue
          }
          const value = addDays(current, number - 1)
          const button = document.createElement('button')
          button.type = 'button'
          button.dataset.ui = 'date-day'
          button.dataset.date = value
          button.textContent = String(number)
          button.setAttribute('aria-label', formatter.format(civil(value)))
          if (value === today) button.setAttribute('aria-current', 'date')
          if (range && draft[0] && !draft[1]) {
            const [start, finish] = orderedRange(draft[0], value)
            button.disabled = !allowed(inputs[0], start) || !allowed(end, finish)
          } else button.disabled = !allowed(inputs[0], value)
          button.tabIndex = value === focusDate && !button.disabled ? 0 : -1
          cell.append(button)
        }
      }
      section.append(table)
      months.append(section)
    }
    months.scrollTop = scrollTop
    const focusable =
      months.querySelector('button[tabindex="0"]') ?? months.querySelector('button:not(:disabled)')
    if (focusable instanceof HTMLButtonElement) {
      focusable.tabIndex = 0
      focusDate = focusable.dataset.date ?? focusDate
      if (focus) {
        focusable.focus({ preventScroll: true })
        revealDay(focusable)
      }
    }
    for (const command of ['previous', 'next']) {
      const button = /** @type {HTMLButtonElement} */ (popup.querySelector(`[value="${command}"]`))
      button.disabled = !moveMonth(
        range && isMobile() ? monthStart(focusDate) : month,
        command === 'previous' ? -1 : range && isMobile() ? 1 : monthCount(),
      )
    }
    if (apply) apply.disabled = !valid()
    status.textContent = draft[0]
      ? range && !draft[1]
        ? `${short.format(civil(draft[0]))} · ${labels.chooseEnd}`
        : `${short.format(civil(draft[0]))}${range ? ` — ${short.format(civil(draft[1]))}` : ''}`
      : range
        ? labels.chooseStart
        : labels.chooseDate
    paint(focus && range && draft[0] && !draft[1] ? focusDate : '')
    position()
  }
  /** @param {HTMLElement} [source] */
  const open = (source = opener) => {
    if (!enabled()) return
    returnFocus = source
    today = readToday()
    draft = inputs.map((input) => (parseDate(input.value) ? input.value : ''))
    focusDate = draft[0] || today
    if (parseDate(inputs[0].min) && focusDate < inputs[0].min) focusDate = inputs[0].min
    if (parseDate(inputs[0].max) && focusDate > inputs[0].max) focusDate = inputs[0].max
    month = calendarStart(focusDate)
    render()
    popup.showPopover()
    syncPresentation()
    opener.setAttribute('aria-expanded', 'true')
    position()
    revealMonth()
    const first = months.querySelector('button[tabindex="0"]')
    if (first instanceof HTMLElement) {
      first.focus({ preventScroll: true })
      revealDay(first)
    } else {
      const cancel = popup.querySelector('[value="cancel"]')
      if (cancel instanceof HTMLElement) cancel.focus()
    }
  }
  /** @param {boolean} [restore] */
  const close = (restore = false) => {
    if (isOpen()) popup.hidePopover()
    syncPresentation()
    opener.setAttribute('aria-expanded', 'false')
    if (restore && returnFocus.isConnected) returnFocus.focus({ preventScroll: true })
  }
  /** @param {Event} event */
  const click = (event) => {
    if (event.target === display && display instanceof HTMLElement) {
      isOpen() ? close(true) : open(display)
      return
    }
    const target = event.target instanceof Element ? event.target.closest('button') : null
    if (!(target instanceof HTMLButtonElement) || target.disabled) return
    if (target === opener) {
      isOpen() ? close(true) : open()
      return
    }
    if (!popup.contains(target) || !enabled()) return
    if (target.dataset.date) {
      const value = target.dataset.date
      preferredPreset = ''
      draft = range ? (draft[0] && !draft[1] ? orderedRange(draft[0], value) : [value, '']) : [value]
      focusDate = value
      render(true)
      if (!range && autoApply) {
        // Commit before the user can submit the containing form. Waiting for
        // the asynchronous popover toggle could submit the previous date.
        commit([...draft])
        close(true)
        submitFilter()
      }
    } else if (target.name === 'kv-date-command') {
      if (target.value === 'cancel' || target.value === 'close') close(true)
      else if (target.value === 'apply' && valid()) {
        const values = [...draft]
        close(true)
        commit(values)
      } else if (target.value === 'previous' || target.value === 'next') {
        const next = moveMonth(
          range && isMobile() ? visibleMonth() : month,
          target.value === 'previous' ? -1 : 1,
        )
        if (!next) return
        focusDate = next
        month = range && isMobile() ? calendarStart(next) : next
        render()
        revealMonth()
      }
    }
  }
  /** @param {KeyboardEvent} event */
  const keydown = (event) => {
    if (
      !isOpen() &&
      event.target === display &&
      display instanceof HTMLElement &&
      ['Enter', ' ', 'ArrowDown'].includes(event.key)
    ) {
      event.preventDefault()
      open(display)
      return
    }
    // A typed date opens its calendar from the keyboard as the native field did.
    if (!isOpen() && entry && event.target === entry && event.key === 'ArrowDown') {
      event.preventDefault()
      open(entry)
      return
    }
    if (!isOpen()) return
    if (event.key === 'Escape') {
      event.preventDefault()
      event.stopImmediatePropagation()
      close(true)
      return
    }
    if (event.key === 'Tab' && isMobile()) {
      const items = [
        ...popup.querySelectorAll('button:not(:disabled):not([tabindex="-1"]), select:not(:disabled)'),
      ]
      const first = items[0]
      const last = items.at(-1)
      if (event.shiftKey && document.activeElement === first && last instanceof HTMLElement) {
        event.preventDefault()
        last.focus()
        return
      }
      if (!event.shiftKey && document.activeElement === last && first instanceof HTMLElement) {
        event.preventDefault()
        first.focus()
        return
      }
    }
    const target = event.target
    if (!(target instanceof HTMLButtonElement) || !target.dataset.date) return
    const value = target.dataset.date
    const weekday = (civil(value).getUTCDay() - weekStart + 7) % 7
    const offsets = {
      ArrowLeft: -1,
      ArrowRight: 1,
      ArrowUp: -7,
      ArrowDown: 7,
      Home: -weekday,
      End: 6 - weekday,
    }
    let next =
      event.key in offsets
        ? addDays(value, offsets[/** @type {keyof typeof offsets} */ (event.key)])
        : event.key === 'PageUp' || event.key === 'PageDown'
          ? moveMonth(value, (event.key === 'PageUp' ? -1 : 1) * (event.shiftKey ? 12 : 1))
          : ''
    if (!next) return
    event.preventDefault()
    for (let attempt = 0; attempt < 366 && next; attempt++) {
      const period = range && draft[0] && !draft[1] ? orderedRange(draft[0], next) : [next]
      if (allowed(inputs[0], period[0]) && (!period[1] || allowed(end, period[1]))) break
      next = addDays(next, next < value ? -1 : 1)
    }
    const period = range && draft[0] && !draft[1] ? orderedRange(draft[0], next) : [next]
    if (!next || !allowed(inputs[0], period[0]) || (period[1] && !allowed(end, period[1]))) return
    focusDate = next
    if (next < month || next > monthEnd(moveMonth(month, monthCount() - 1))) month = monthStart(next)
    render(true)
  }
  /** @param {PointerEvent} event */
  const hover = (event) => {
    const target = event.target
    if (range && draft[0] && !draft[1] && target instanceof HTMLButtonElement && !target.disabled)
      paint(target.dataset.date)
  }
  const leave = () => paint()
  let disposing = false
  const autoApply = picker.dataset.dateApplyOnClose === 'true'
  const toggle = () => {
    syncPresentation()
    opener.setAttribute('aria-expanded', String(isOpen()))
    if (!isOpen()) {
      if (autoApply && !disposing && draft.length) {
        const changed = valid() && inputs.some((input, index) => input.value !== draft[index])
        if (changed) {
          commit([...draft])
          submitFilter()
        } else validate()
      }
      draft = []
    }
  }
  /** @param {FocusEvent} event */
  const focusout = (event) => {
    if (isOpen() && event.target instanceof Node && !popup.contains(event.target) && event.target !== opener)
      close()
  }
  let mobileLayout = isMobile()
  const resize = () => {
    if (isOpen()) {
      const changed = mobileLayout !== isMobile()
      if (changed) month = calendarStart(focusDate)
      syncPresentation()
      render(months.contains(document.activeElement))
      if (changed) revealMonth()
    }
    mobileLayout = isMobile()
  }
  /** @param {Event} event */
  const change = (event) => {
    if (event.target === preset && preset instanceof HTMLSelectElement && enabled()) {
      today = readToday()
      const period = presetRange(preset.value, today)
      if (period && allowed(inputs[0], period[0]) && allowed(end, period[1])) {
        preferredPreset = preset.value
        if (autoApply) {
          draft = [...period]
          close(true)
        } else {
          close(true)
          commit(period)
        }
      } else {
        validate()
        open(preset)
      }
    } else validate()
  }
  /** @param {SubmitEvent} event */
  const submit = (event) => {
    if (
      !validate() &&
      !inputs[0].form?.noValidate &&
      !(event.submitter instanceof HTMLButtonElement && event.submitter.formNoValidate)
    ) {
      event.preventDefault()
      field?.focus()
    }
  }
  const reset = () => {
    draft = []
    close()
    setTimeout(() => {
      // A hidden field keeps what was last set, so the typed date returns to the saved one by hand too.
      if (range || entry)
        inputs.forEach((input, index) => {
          input.value = defaults[index]
        })
      showEntry()
      preferredPreset = ''
      validate()
    })
  }
  picker.dataset.dateEnhanced = 'true'
  // A CSR refresh can replace the saved boundaries without firing a DOM change.
  // The read-only display may be dirty after a calendar commit, so updating its
  // value attribute alone does not update the visible value in the browser.
  const rangeValues = range && typeof MutationObserver !== 'undefined' ? new MutationObserver(validate) : null
  rangeValues?.observe(picker, { attributes: true, subtree: true, attributeFilter: ['value'] })
  let renderedValue = picker.dataset.dateValue ?? ''
  const singleValue =
    !range && typeof MutationObserver !== 'undefined'
      ? new MutationObserver(() => {
          const value = picker.dataset.dateValue ?? ''
          // A renderer may assign the same attribute again during an unrelated
          // update. Preserve the user's native draft until the supplied value changes.
          if (value === renderedValue) return
          renderedValue = value
          if (inputs[0].value !== value) inputs[0].value = value
          showEntry()
        })
      : null
  singleValue?.observe(picker, { attributes: true, attributeFilter: ['data-date-value'] })
  // An island can lock a saved record without replacing the field.
  const entryControls =
    entry && typeof MutationObserver !== 'undefined' ? new MutationObserver(syncEntryControls) : null
  if (entry && entryBox instanceof HTMLElement) {
    native.type = 'hidden'
    entryBox.hidden = false
    for (const label of document.querySelectorAll('label')) {
      if (label.htmlFor !== native.id || !native.id) continue
      labelled.push([label, native.id])
      label.htmlFor = entry.id
    }
    syncEntryControls()
    showEntry()
    entryControls?.observe(native, { attributes: true, attributeFilter: ['disabled', 'readonly'] })
    picker.addEventListener('input', typing)
    picker.addEventListener('change', typing)
  }
  validate()
  picker.addEventListener('click', click)
  picker.addEventListener('keydown', keydown)
  picker.addEventListener('change', change)
  popup.addEventListener('toggle', toggle)
  months.addEventListener('pointerover', hover)
  months.addEventListener('pointerleave', leave)
  document.addEventListener('focusin', focusout)
  window.addEventListener('resize', resize)
  window.addEventListener('scroll', position, true)
  inputs[0].form?.addEventListener('reset', reset)
  inputs[0].form?.addEventListener('submit', submit)
  return () => {
    disposing = true
    rangeValues?.disconnect()
    singleValue?.disconnect()
    entryControls?.disconnect()
    if (entry && entryBox instanceof HTMLElement) {
      picker.removeEventListener('input', typing)
      picker.removeEventListener('change', typing)
      native.type = 'date'
      entryBox.hidden = true
      for (const [label, id] of labelled) label.htmlFor = id
    }
    close()
    delete picker.dataset.dateEnhanced
    picker.removeEventListener('click', click)
    picker.removeEventListener('keydown', keydown)
    picker.removeEventListener('change', change)
    popup.removeEventListener('toggle', toggle)
    months.removeEventListener('pointerover', hover)
    months.removeEventListener('pointerleave', leave)
    document.removeEventListener('focusin', focusout)
    window.removeEventListener('resize', resize)
    window.removeEventListener('scroll', position, true)
    inputs[0].form?.removeEventListener('reset', reset)
    inputs[0].form?.removeEventListener('submit', submit)
    if (preset instanceof HTMLSelectElement) preset.disabled = true
  }
}
/** Shared ownership prevents duplicate listeners when document and island roots overlap.
 * @type {WeakMap<HTMLElement, { users: number, cleanup: () => void }>} */
const instances = new WeakMap()
/** @param {HTMLElement} node */
const acquire = (node) => {
  let instance = instances.get(node)
  if (!instance) {
    instance = { users: 0, cleanup: enhance(node) }
    instances.set(node, instance)
  }
  const owned = instance
  owned.users++
  return () => {
    if (--owned.users === 0) {
      owned.cleanup()
      instances.delete(node)
    }
  }
}
/** Enhances current and subsequently inserted fields; cleanup also releases removed islands.
 * @param {ParentNode} root */
const attachDatePickers = (root) => {
  if (typeof HTMLElement === 'undefined' || typeof Node === 'undefined') return () => {}
  /** @type {Map<HTMLElement, () => void>} */
  const attached = new Map()
  const sync = () => {
    for (const [element, cleanup] of attached)
      if (element !== root && !root.contains(element)) {
        cleanup()
        attached.delete(element)
      }
    const candidates = [...root.querySelectorAll('[data-date-mode]')]
    if (root instanceof HTMLElement && root.matches('[data-date-mode]')) candidates.unshift(root)
    for (const node of candidates)
      if (node instanceof HTMLElement && !attached.has(node)) attached.set(node, acquire(node))
  }
  sync()
  const observer = typeof MutationObserver === 'undefined' ? null : new MutationObserver(sync)
  if (root instanceof Node) observer?.observe(root, { childList: true, subtree: true })
  return () => {
    observer?.disconnect()
    for (const cleanup of attached.values()) cleanup()
    attached.clear()
  }
}

return attachDatePickers;
})();
// @ts-check

const focusableSelector = [
  'a[href]',
  'button:not([disabled])',
  'input:not([disabled])',
  'select:not([disabled])',
  'textarea:not([disabled])',
  'summary',
  '[tabindex]:not([tabindex="-1"])',
].join(',')

/** @param {ParentNode} root */
const focusables = (root) =>
  [...root.querySelectorAll(focusableSelector)].filter(
    (element) =>
      element instanceof HTMLElement &&
      element.tabIndex >= 0 &&
      !element.matches(':disabled') &&
      !element.closest('[hidden], [inert], [aria-hidden="true"], details:not([open]) > :not(summary)') &&
      (typeof element.checkVisibility !== 'function' ||
        element.checkVisibility({ visibilityProperty: true })),
  )

/**
 * Where focus lands when a route modal opens. A modal that comes back from a rejected submit
 * starts on its first invalid control, so the person lands on what to fix; otherwise on the
 * first focusable control, or the dialog itself.
 * @param {HTMLElement} modal
 */
const routeModalFocusTarget = (modal) => {
  const controls = focusables(modal)
  for (const control of controls) {
    if (control instanceof HTMLElement && control.getAttribute('aria-invalid') === 'true') return control
  }
  return controls[0] instanceof HTMLElement ? controls[0] : modal
}

/**
 * A menu panel opens from its trigger's start or end. Where that carries it out of the viewport,
 * as a trigger in the middle of a phone row does, the panel moves along the row until it is 8px
 * inside, its leading edge on a whole pixel; a panel the viewport holds stays where its trigger put it.
 * @param {HTMLElement} panel
 * @param {boolean} open
 * @param {{ left: number, right: number }} [clip]
 */
const keepMenuPanelInView = (
  panel,
  open,
  clip = { left: 8, right: document.documentElement.clientWidth - 8 },
) => {
  panel.style.removeProperty('translate')
  panel.style.removeProperty('--kv-menu-shift-inline')
  if (!open) return
  const box = panel.getBoundingClientRect()
  const shift = Math.max(clip.left - box.left, Math.min(0, clip.right - box.right))
  if (!shift) return
  const left = shift < 0 ? Math.floor(box.left + shift) : Math.ceil(box.left + shift)
  panel.style.setProperty('--kv-menu-shift-inline', `${left - box.left}px`)
}

/**
 * Attach the small browser contract shared by server-rendered overlays.
 * The returned function restores inert state and removes every listener.
 *
 * @param {ParentNode} [root]
 * @returns {() => void}
 */
/**
 * Popups built on <details> other than the action menu: a period filter, view
 * settings and a list toolbar's folded filters. They close on a click outside
 * them and on Escape, like the menu.
 */
const DISMISSIBLE_POPUPS = [
  '[data-ui="timeframe-menu"]',
  '[data-ui="view-settings"]',
  '[data-ui="list-filters"]',
]
const DISMISSIBLE_POPUPS_OPEN = DISMISSIBLE_POPUPS.map((selector) => `${selector}[open]`).join(', ')

/** @param {ParentNode} root */
const attachGlobalSearch = (root) => {
  /** @type {(() => void)[]} */
  const cleanups = []
  const launchers = [...root.querySelectorAll('[data-ui="global-search-trigger"]')]
  /** @type {(() => boolean)[]} */
  const openers = []
  for (const launcher of launchers) {
    const dialog = [...root.querySelectorAll('[data-ui="global-search-dialog"]')].find(
      (candidate) => candidate.id === launcher.getAttribute('aria-controls'),
    )
    if (!(launcher instanceof HTMLElement) || !(dialog instanceof HTMLDialogElement)) continue
    const input = dialog.querySelector('[data-ui="global-search-input"]')
    const form = dialog.querySelector('[data-ui="global-search"]')
    const template = dialog.querySelector('template')
    if (
      !(input instanceof HTMLInputElement) ||
      !(form instanceof HTMLElement) ||
      !(template instanceof HTMLTemplateElement)
    )
      continue
    const layer = template.content.querySelector('[data-ui="modal-layer"]')
    const body = template.content.querySelector('[data-ui="modal-body"]')
    if (!layer || !body) continue
    const parkContent = () => {
      dialog.append(form)
      template.content.append(layer)
    }
    const shortcut = launcher.querySelector('[data-ui="global-search-shortcut"]')
    if (shortcut instanceof HTMLElement) {
      shortcut.textContent = /Mac|iPhone|iPad/.test(navigator.platform) ? '⌘ K' : 'Ctrl K'
      shortcut.hidden = false
    }
    const open = () => {
      if (launcher.closest('[inert]')) return false
      if (!dialog.open) {
        if (root.querySelector('dialog[open], [data-ui="modal-layer"][data-route-modal="true"]')) return false
        dialog.append(layer)
        body.append(form)
        dialog.showModal()
      }
      launcher.setAttribute('aria-expanded', 'true')
      input.focus()
      input.select()
      return true
    }
    openers.push(open)
    const restore = () => {
      parkContent()
      launcher.setAttribute('aria-expanded', 'false')
      if (launcher.isConnected) launcher.focus()
    }
    /** @param {MouseEvent} event */
    const launch = (event) => {
      if (event.button !== 0 || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return
      if (open()) event.preventDefault()
    }
    /** @param {Event} event */
    const dismiss = (event) => {
      if (
        event.target instanceof Element &&
        event.target.closest('[data-ui="modal-close"], [data-ui="modal-backdrop"]')
      ) {
        dialog.close()
      }
    }
    /** @param {Event} event */
    const cancel = (event) => {
      event.preventDefault()
      event.stopPropagation()
      dialog.close()
    }
    const submit = () => dialog.close()
    launcher.addEventListener('click', launch)
    dialog.addEventListener('click', dismiss)
    dialog.addEventListener('cancel', cancel)
    dialog.addEventListener('close', restore)
    dialog.addEventListener('submit', submit)
    cleanups.push(() => {
      launcher.removeEventListener('click', launch)
      dialog.removeEventListener('click', dismiss)
      dialog.removeEventListener('cancel', cancel)
      dialog.removeEventListener('close', restore)
      dialog.removeEventListener('submit', submit)
      if (dialog.open) dialog.close()
      parkContent()
    })
  }
  /** @param {KeyboardEvent} event */
  const shortcut = (event) => {
    if (
      (event.ctrlKey || event.metaKey) &&
      event.key.toLowerCase() === 'k' &&
      !event.altKey &&
      !event.defaultPrevented
    ) {
      if (openers.some((open) => open())) event.preventDefault()
    }
  }
  document.addEventListener('keydown', shortcut)
  return () => {
    document.removeEventListener('keydown', shortcut)
    for (const cleanup of cleanups) cleanup()
  }
}

/**
 * Binds the interactions of every component under `root`: the whole page, or one island's element.
 * @param {Document | HTMLElement} [root]
 */
const attachDesignSystemInteractions = (root = document) => {
  const cleanupNavigationTools = attachNavigationTools(root)
  const cleanupPrimitives = attachPrimitives(root)
  const cleanupDatePickers = attachDatePickers(root)
  const cleanupGlobalSearch = attachGlobalSearch(root)
  const cleanupApplyOnInput = attachApplyOnInput(document)
  const cleanupNumberEntry = attachNumberEntry(document)
  const activeBeforeOpen = document.activeElement instanceof HTMLElement ? document.activeElement : null
  const modal = root.querySelector('[data-ui="modal-layer"][data-route-modal="true"] [role="dialog"]')
  const appShell = root.querySelector('[data-ui="app-shell"], [data-ui="shell"]')
  const priorInert = appShell instanceof HTMLElement ? appShell.inert : false
  const ownsShellLock =
    modal instanceof HTMLElement && appShell instanceof HTMLElement && !appShell.contains(modal)
  if (modal instanceof HTMLElement) {
    if (ownsShellLock && appShell instanceof HTMLElement) appShell.inert = true
    routeModalFocusTarget(modal).focus()
  }

  // A list toolbar's folded filters are a disclosure with a button trigger, as a menu is.
  const menuDisclosures = '[data-ui="menu"], [data-ui="list-filters"]'
  const menuProperties = [
    '--kv-menu-shift-inline',
    '--kv-menu-shift-block',
    '--kv-menu-available-block',
    '--kv-menu-available-inline',
  ]
  /** @type {Map<HTMLDetailsElement, ResizeObserver>} */
  const menuObservers = new Map()
  const pruneMenuObservers = () => {
    for (const [menu, observer] of menuObservers) {
      if (menu.isConnected) continue
      observer.disconnect()
      menuObservers.delete(menu)
    }
  }
  /** @param {HTMLDetailsElement} menu */
  const syncMenu = (menu) => {
    // Its own trigger: folded filters hold menus whose triggers are not theirs.
    const trigger = menu.querySelector(':scope > summary')
    if (trigger instanceof HTMLElement) trigger.setAttribute('aria-expanded', String(menu.open))
    const panel = menu.querySelector(':scope > [data-ui="menu-panel"]')
    if (!(panel instanceof HTMLElement) || !(trigger instanceof HTMLElement)) return
    for (const property of menuProperties) panel.style.removeProperty(property)
    if (!menu.open) {
      menuObservers.get(menu)?.disconnect()
      menuObservers.delete(menu)
      return
    }
    if (typeof ResizeObserver !== 'undefined' && !menuObservers.has(menu)) {
      const observer = new ResizeObserver(() => {
        pruneMenuObservers()
        if (menu.isConnected) syncMenu(menu)
      })
      menuObservers.set(menu, observer)
      observer.observe(panel)
    }
    const tokens = getComputedStyle(panel)
    const viewport = panel.ownerDocument.documentElement
    const rootSize = Number.parseFloat(getComputedStyle(viewport).fontSize) || 16
    /** @param {string} key */
    const tokenPixels = (key) => {
      const value = tokens.getPropertyValue(key).trim()
      return (Number.parseFloat(value) || 0) * (value.endsWith('rem') ? rootSize : 1)
    }
    const inset = tokenPixels('--kv-space-2')
    const gap = tokenPixels('--kv-space-1')
    const clip = {
      left: inset,
      top: inset,
      right: viewport.clientWidth - inset,
      bottom: viewport.clientHeight - inset,
    }
    for (let parent = menu.parentElement; parent; parent = parent.parentElement) {
      const styles = getComputedStyle(parent)
      const bounds = parent.getBoundingClientRect()
      if (/auto|scroll|hidden|clip/.test(styles.overflowX)) {
        clip.left = Math.max(clip.left, bounds.left + inset)
        clip.right = Math.min(clip.right, bounds.right - inset)
      }
      if (/auto|scroll|hidden|clip/.test(styles.overflowY)) {
        clip.top = Math.max(clip.top, bounds.top + inset)
        clip.bottom = Math.min(clip.bottom, bounds.bottom - inset)
      }
    }
    panel.style.setProperty('--kv-menu-available-inline', `${Math.max(0, clip.right - clip.left)}px`)
    const anchor = trigger.getBoundingClientRect()
    if (menu.dataset.placement === 'right') {
      panel.style.setProperty('--kv-menu-available-block', `${Math.max(0, clip.bottom - clip.top)}px`)
      const bounds = panel.getBoundingClientRect()
      const top = Math.max(clip.top, Math.min(anchor.bottom - bounds.height, clip.bottom - bounds.height))
      panel.style.setProperty('--kv-menu-shift-block', `${top - bounds.top}px`)
      keepMenuPanelInView(panel, menu.open, clip)
      return
    }
    const initial = panel.getBoundingClientRect()
    const below = clip.bottom - anchor.bottom - gap
    const above = anchor.top - gap - clip.top
    const prefersAbove = menu.dataset.placement === 'top'
    const useAbove = prefersAbove
      ? above >= Math.min(initial.height, below)
      : below < initial.height && above > below
    panel.style.setProperty('--kv-menu-available-block', `${Math.max(0, useAbove ? above : below)}px`)
    const bounds = panel.getBoundingClientRect()
    const top = useAbove ? anchor.top - gap - bounds.height : anchor.bottom + gap
    panel.style.setProperty('--kv-menu-shift-block', `${top - bounds.top}px`)
    keepMenuPanelInView(panel, menu.open, clip)
  }
  // A toggle does not bubble; capturing also follows a menu an island draws later.
  /** @param {Event} event */
  const onMenuToggle = (event) => {
    pruneMenuObservers()
    const menu = event.target
    if (menu instanceof HTMLDetailsElement && menu.matches(menuDisclosures)) syncMenu(menu)
  }
  const onMenuResize = () => {
    pruneMenuObservers()
    for (const menu of root.querySelectorAll('[data-ui="menu"][open]')) {
      if (menu instanceof HTMLDetailsElement) syncMenu(menu)
    }
  }
  for (const menu of root.querySelectorAll(menuDisclosures)) {
    if (menu instanceof HTMLDetailsElement) syncMenu(menu)
  }
  root.addEventListener('toggle', onMenuToggle, true)
  window.addEventListener('resize', onMenuResize)

  const navigationMedia = window.matchMedia('(width < 48rem)')
  const navigationRoot = document.documentElement
  const priorNavigationOpen = navigationRoot.dataset.kvNavigationOpen
  const navigations = /** @type {HTMLDetailsElement[]} */ (
    [...root.querySelectorAll('[data-ui="app-navigation"]')].filter(
      (navigation) => navigation instanceof HTMLDetailsElement,
    )
  )
  // Keep one SSR app rail. On mobile it belongs inside the dialog focus boundary.
  const appRails = [...root.querySelectorAll('[data-ui="app-shell"] > [data-ui="app-switcher"]')].map(
    (rail) => ({ rail, parent: rail.parentNode, next: rail.nextSibling }),
  )
  const restoreAppRails = () => {
    for (const { rail, parent, next } of appRails) {
      if (parent && rail.parentNode !== parent)
        parent.insertBefore(rail, next?.parentNode === parent ? next : null)
    }
  }
  const syncAppRails = () => {
    if (!navigationMedia.matches) {
      restoreAppRails()
      return
    }
    for (const { rail, parent } of appRails) {
      const drawer = parent?.querySelector('[data-ui="navigation-drawer"]')
      if (drawer && rail.parentNode !== drawer) drawer.prepend(rail)
    }
  }
  syncAppRails()
  const navigationStates = new Map()

  /** @param {HTMLDetailsElement} navigation */
  const restoreNavigationTargets = (navigation) => {
    const state = navigationStates.get(navigation)
    if (!state?.targets) return
    for (const { element, inert } of state.targets) element.inert = inert
    state.targets = null
  }

  const syncNavigationLock = () => {
    const hasOpenNavigation = navigationMedia.matches && navigations.some((navigation) => navigation.open)
    if (hasOpenNavigation) navigationRoot.dataset.kvNavigationOpen = 'true'
    else if (priorNavigationOpen === undefined) delete navigationRoot.dataset.kvNavigationOpen
    else navigationRoot.dataset.kvNavigationOpen = priorNavigationOpen
  }

  /** @param {HTMLDetailsElement} navigation */
  const syncNavigation = (navigation) => {
    syncAppRails()
    const trigger = navigation.querySelector('[data-ui="navigation-trigger"]')
    const drawer = navigation.querySelector('[data-ui="navigation-drawer"]')
    if (!(trigger instanceof HTMLElement) || !(drawer instanceof HTMLElement)) return
    const mobile = navigationMedia.matches
    trigger.dataset.open = String(mobile && navigation.open)
    for (const toggle of root.querySelectorAll('[data-ui="navigation-toggle"]')) {
      if (toggle.getAttribute('aria-controls') === drawer.id)
        toggle.setAttribute('aria-expanded', String(mobile && navigation.open))
    }
    if (!mobile) {
      drawer.removeAttribute('role')
      drawer.removeAttribute('aria-modal')
      drawer.removeAttribute('aria-label')
      restoreNavigationTargets(navigation)
      syncNavigationLock()
      return
    }
    drawer.setAttribute('role', 'dialog')
    drawer.setAttribute('aria-modal', 'true')
    drawer.setAttribute('aria-label', drawer.dataset.navigationLabel ?? 'Navigation')
    if (!navigation.open) {
      for (const menu of drawer.querySelectorAll('[data-ui="menu"][open]')) {
        if (menu instanceof HTMLDetailsElement) menu.open = false
      }
      restoreNavigationTargets(navigation)
      syncNavigationLock()
      return
    }
    const state = navigationStates.get(navigation) ?? {}
    if (!state.targets) {
      const shell = navigation.closest('[data-ui="app-shell"], [data-ui="shell"]')
      const targets = shell
        ? [
            ...shell.querySelectorAll(
              ':scope > [data-ui="app-main"], :scope > [data-ui="main"], :scope > [data-ui="app-right-rail"], :scope > [data-ui="app-shell-topbar"]',
            ),
          ]
            .filter((element) => element instanceof HTMLElement)
            .map((element) => ({ element, inert: element.inert }))
        : []
      state.returnFocus = document.activeElement instanceof HTMLElement ? document.activeElement : trigger
      for (const { element } of targets) element.inert = true
      state.targets = targets
      navigationStates.set(navigation, state)
      queueMicrotask(() => {
        if (!navigation.open || !navigationMedia.matches) return
        const activeItem = drawer.querySelector('[data-ui="navigation-item"][aria-current="page"]')
        const first = focusables(drawer)[0]
        const focusTarget = activeItem instanceof HTMLElement ? activeItem : first
        ;(focusTarget instanceof HTMLElement ? focusTarget : drawer).focus()
      })
    }
    syncNavigationLock()
  }

  /** @param {HTMLDetailsElement} navigation @param {boolean} [returnFocus] */
  const closeNavigation = (navigation, returnFocus = true) => {
    const state = navigationStates.get(navigation)
    navigation.open = false
    syncNavigation(navigation)
    if (returnFocus && state?.returnFocus instanceof HTMLElement && state.returnFocus.isConnected)
      state.returnFocus.focus()
    navigationStates.delete(navigation)
  }

  const navigationCleanups = navigations.map((navigation) => {
    const onToggle = () => syncNavigation(navigation)
    /** @param {MouseEvent} event */
    const onClick = (event) => {
      const target = event.target
      if (!(target instanceof Element)) return
      const branchTrigger = target.closest('[data-ui="navigation-branch-trigger"]')
      const branch = branchTrigger?.closest('[data-ui="navigation-branch"]')
      if (branch instanceof HTMLDetailsElement && branch.open && branch.hasAttribute('name')) {
        event.preventDefault()
        return
      }
      if (!navigationMedia.matches || !navigation.open) return
      if (
        target.closest('[data-ui="navigation-close"]') ||
        target.closest('[data-ui="navigation-backdrop"]') ||
        target.closest('[data-ui="navigation-item"]')
      )
        closeNavigation(navigation)
    }
    navigation.addEventListener('toggle', onToggle)
    navigation.addEventListener('click', onClick)
    syncNavigation(navigation)
    return () => {
      navigation.removeEventListener('toggle', onToggle)
      navigation.removeEventListener('click', onClick)
      restoreNavigationTargets(navigation)
    }
  })
  const onNavigationMediaChange = () => {
    for (const navigation of navigations) {
      if (navigation.open) closeNavigation(navigation, false)
      else syncNavigation(navigation)
    }
  }
  navigationMedia.addEventListener('change', onNavigationMediaChange)

  for (const popover of root.querySelectorAll('[data-ui="popover"][data-open="true"]')) {
    const trigger = popover.querySelector('[data-ui="popover-trigger"]')
    const panel = popover.querySelector('[data-ui="popover-panel"]')
    if (!(trigger instanceof HTMLElement) || !(panel instanceof HTMLElement)) continue
    const box = trigger.getBoundingClientRect()
    const gap = 8
    const placement = popover.getAttribute('data-placement') ?? 'bottom-start'
    const prefersTop = placement.startsWith('top')
    const prefersEnd = placement.endsWith('end')
    const topPosition = box.top - panel.offsetHeight - gap
    const bottomPosition = box.bottom + gap
    const topFits = topPosition >= 8
    const bottomFits = bottomPosition + panel.offsetHeight <= window.innerHeight - 8
    const useTop = prefersTop ? topFits || !bottomFits : !bottomFits && topFits
    const requestedLeft = prefersEnd ? box.right - panel.offsetWidth : box.left
    const left = Math.max(8, Math.min(requestedLeft, window.innerWidth - panel.offsetWidth - 8))
    const top = Math.max(
      8,
      Math.min(useTop ? topPosition : bottomPosition, window.innerHeight - panel.offsetHeight - 8),
    )
    panel.dataset.runtimePositioned = 'true'
    panel.dataset.runtimePlacement = `${useTop ? 'top' : 'bottom'}-${prefersEnd ? 'end' : 'start'}`
    panel.style.left = `${Math.round(left)}px`
    panel.style.top = `${Math.round(top)}px`
    panel.style.right = 'auto'
    panel.style.bottom = 'auto'
  }

  /** @param {KeyboardEvent} event */
  const onKeydown = (event) => {
    // A page and its islands may both attach the runtime. Only the owning root
    // handles a key, and a key consumed by another layer must not move focus twice.
    if (event.defaultPrevented || !root.contains(document.activeElement)) return
    // Enter in a short entry with a command beside it (Field `action`) runs that command, not the
    // form's first submit button.
    if (event.key === 'Enter' && !event.isComposing && event.target instanceof HTMLInputElement) {
      const command = event.target.closest('[data-ui="field-row"]')?.lastElementChild
      const button = command?.closest('button') ?? command?.querySelector('button')
      if (button instanceof HTMLButtonElement && !button.disabled) {
        event.preventDefault()
        button.click()
        return
      }
    }
    // Native dialogs own Escape and focus traversal, including ones outside this runtime root.
    // Keep the composed path: another listener may synchronously close/remove the dialog.
    if (
      (event.key === 'Escape' || event.key === 'Tab') &&
      event.composedPath().some((target) => target instanceof HTMLDialogElement)
    )
      return
    const openNavigation = navigations.find((navigation) => navigationMedia.matches && navigation.open)
    if (openNavigation instanceof HTMLDetailsElement) {
      const drawer = openNavigation.querySelector('[data-ui="navigation-drawer"]')
      if (event.key === 'Escape' && !document.activeElement?.closest('[data-ui="menu"][open]')) {
        closeNavigation(openNavigation)
        event.preventDefault()
        return
      }
      if (event.key === 'Tab' && drawer instanceof HTMLElement) {
        const items = focusables(drawer)
        const first = items[0]
        const last = items.at(-1)
        if (!items.length) {
          event.preventDefault()
          drawer.focus()
          return
        }
        if (event.shiftKey && (document.activeElement === first || document.activeElement === drawer)) {
          event.preventDefault()
          if (last instanceof HTMLElement) last.focus()
          return
        }
        if (!event.shiftKey && document.activeElement === last) {
          event.preventDefault()
          if (first instanceof HTMLElement) first.focus()
          return
        }
      }
    }
    const treeItem = document.activeElement?.closest('[data-ui="tree"] [role="treeitem"]')
    if (treeItem instanceof HTMLElement) {
      const tree = treeItem.closest('[data-ui="tree"]')
      const items = /** @type {HTMLElement[]} */ (
        tree
          ? [...tree.querySelectorAll('[role="treeitem"]')].filter((item) => item instanceof HTMLElement)
          : []
      )
      const activeIndex = items.indexOf(treeItem)
      let nextIndex = -1
      if (event.key === 'ArrowDown') nextIndex = Math.min(items.length - 1, activeIndex + 1)
      else if (event.key === 'ArrowUp') nextIndex = Math.max(0, activeIndex - 1)
      else if (event.key === 'Home') nextIndex = 0
      else if (event.key === 'End') nextIndex = items.length - 1
      else if (event.key === 'ArrowRight') {
        const level = Number(treeItem.getAttribute('aria-level'))
        if (
          treeItem.getAttribute('aria-expanded') === 'true' &&
          Number(items[activeIndex + 1]?.getAttribute('aria-level')) > level
        )
          nextIndex = activeIndex + 1
      } else if (event.key === 'ArrowLeft') {
        const level = Number(treeItem.getAttribute('aria-level'))
        if (level > 1)
          nextIndex = items.findLastIndex(
            (item, index) => index < activeIndex && Number(item.getAttribute('aria-level')) < level,
          )
      }
      if (nextIndex >= 0 && items[nextIndex]) {
        for (const item of items) item.tabIndex = -1
        items[nextIndex].tabIndex = 0
        items[nextIndex].focus()
        event.preventDefault()
        return
      }
    }
    const treeGridRow = document.activeElement?.closest('[data-ui="tree-grid-row"]')
    if (treeGridRow instanceof HTMLElement) {
      const grid = treeGridRow.closest('[data-ui="tree-grid"]')
      const rows = /** @type {HTMLElement[]} */ (
        grid
          ? [...grid.querySelectorAll('[data-ui="tree-grid-row"]')].filter(
              (row) => row instanceof HTMLElement,
            )
          : []
      )
      const activeIndex = rows.indexOf(treeGridRow)
      // A focused row opens its record the same way its row link does.
      const rowLink = treeGridRow.querySelector('[data-ui="tree-grid-link"]')
      if (
        event.key === 'Enter' &&
        document.activeElement === treeGridRow &&
        rowLink instanceof HTMLAnchorElement
      ) {
        rowLink.click()
        event.preventDefault()
        return
      }
      const nextIndex =
        event.key === 'ArrowDown'
          ? Math.min(rows.length - 1, activeIndex + 1)
          : event.key === 'ArrowUp'
            ? Math.max(0, activeIndex - 1)
            : event.key === 'Home'
              ? 0
              : event.key === 'End'
                ? rows.length - 1
                : -1
      if (nextIndex >= 0 && rows[nextIndex]) {
        for (const row of rows) row.tabIndex = -1
        rows[nextIndex].tabIndex = 0
        rows[nextIndex].focus()
        event.preventDefault()
        return
      }
    }
    const activeMenu = document.activeElement?.closest('[data-ui="menu"]')
    if (
      activeMenu instanceof HTMLDetailsElement &&
      activeMenu.querySelector(':scope > summary')?.getAttribute('aria-disabled') === 'true' &&
      ['ArrowDown', 'ArrowUp', 'Home', 'End', 'Enter', ' '].includes(event.key)
    ) {
      event.preventDefault()
      return
    }
    if (activeMenu instanceof HTMLDetailsElement && !activeMenu.open) {
      const trigger = activeMenu.querySelector('[data-ui="menu-trigger"]')
      if (document.activeElement === trigger && (event.key === 'ArrowDown' || event.key === 'ArrowUp')) {
        activeMenu.open = true
        const items = /** @type {HTMLElement[]} */ (
          [
            ...activeMenu.querySelectorAll(
              '[role="menuitem"], [role="menuitemcheckbox"], [role="menuitemradio"]',
            ),
          ].filter(
            (item) =>
              item instanceof HTMLElement &&
              item.getAttribute('aria-disabled') !== 'true' &&
              !item.hidden &&
              (typeof item.checkVisibility !== 'function' || item.checkVisibility()),
          )
        )
        const item = event.key === 'ArrowUp' ? items.at(-1) : items[0]
        if (item instanceof HTMLElement) item.focus()
        event.preventDefault()
        return
      }
    }
    const openMenu = document.activeElement?.closest('[data-ui="menu"][open]')
    if (openMenu instanceof HTMLDetailsElement) {
      const trigger = openMenu.querySelector('[data-ui="menu-trigger"]')
      const items = /** @type {HTMLElement[]} */ (
        [
          ...openMenu.querySelectorAll(
            '[role="menuitem"], [role="menuitemcheckbox"], [role="menuitemradio"]',
          ),
        ].filter(
          (item) =>
            item instanceof HTMLElement &&
            item.getAttribute('aria-disabled') !== 'true' &&
            !item.hidden &&
            (typeof item.checkVisibility !== 'function' || item.checkVisibility()),
        )
      )
      const active = document.activeElement
      const activeIndex = active instanceof HTMLElement ? items.indexOf(active) : -1
      let nextIndex = -1
      if (event.key === 'ArrowDown') nextIndex = activeIndex < 0 ? 0 : (activeIndex + 1) % items.length
      else if (event.key === 'ArrowUp')
        nextIndex = activeIndex < 0 ? items.length - 1 : (activeIndex - 1 + items.length) % items.length
      else if (event.key === 'Home') nextIndex = 0
      else if (event.key === 'End') nextIndex = items.length - 1
      if (nextIndex >= 0 && items[nextIndex] instanceof HTMLElement) {
        items[nextIndex].focus()
        event.preventDefault()
        return
      }
      if ((event.key === 'Enter' || event.key === ' ') && document.activeElement === trigger) {
        if (items[0] instanceof HTMLElement) items[0].focus()
        event.preventDefault()
        return
      }
    }
    if (event.key === 'Escape' && openMenu instanceof HTMLDetailsElement) {
      openMenu.open = false
      const trigger = openMenu.querySelector('[data-ui="menu-trigger"]')
      if (trigger instanceof HTMLElement) trigger.focus()
      event.preventDefault()
      return
    }
    const openPopup = [...root.querySelectorAll(DISMISSIBLE_POPUPS_OPEN)].at(-1)
    if (event.key === 'Escape' && openPopup instanceof HTMLDetailsElement) {
      openPopup.open = false
      const summary = openPopup.querySelector('summary')
      if (summary instanceof HTMLElement) summary.focus()
      event.preventDefault()
      return
    }
    const openPopover = document.activeElement?.closest('[data-ui="popover"][data-open="true"]')
    if (event.key === 'Escape' && openPopover instanceof HTMLElement) {
      const close = openPopover.querySelector('[data-ui="popover-close"]')
      if (close instanceof HTMLElement) close.click()
      event.preventDefault()
      return
    }
    if (!(modal instanceof HTMLElement)) return
    if (event.key === 'Escape') {
      const close = modal.querySelector('[data-ui="modal-close"]')
      if (close instanceof HTMLElement) close.click()
      event.preventDefault()
      return
    }
    if (event.key !== 'Tab') return
    const items = focusables(modal)
    if (!items.length) {
      event.preventDefault()
      modal.focus()
      return
    }
    const first = items[0]
    const last = items.at(-1)
    if (event.shiftKey && (document.activeElement === first || document.activeElement === modal)) {
      event.preventDefault()
      if (last instanceof HTMLElement) last.focus()
    } else if (!event.shiftKey && document.activeElement === last) {
      event.preventDefault()
      if (first instanceof HTMLElement) first.focus()
    }
  }
  /**
   * Table selection. `select-all` toggles the enabled row checkboxes of its own table, a row
   * change keeps the header checked or indeterminate, and every bulk bar bound to the same form
   * (`data-form`) shows the live count and enables its actions only while something is selected.
   *
   * @param {Element} table
   */
  const syncSelection = (table) => {
    const rows = /** @type {HTMLInputElement[]} */ (
      [...table.querySelectorAll('[data-ui="row-select"]')].filter(
        (input) => input instanceof HTMLInputElement && !input.disabled,
      )
    )
    const checked = rows.filter((input) => input.checked)
    const all = table.querySelector('[data-ui="select-all"]')
    if (all instanceof HTMLInputElement) {
      all.checked = rows.length > 0 && checked.length === rows.length
      all.indeterminate = checked.length > 0 && checked.length < rows.length
    }
    for (const input of rows)
      input.closest('[data-ui="row"]')?.toggleAttribute('data-selected', input.checked)
    const form = rows[0]?.getAttribute('form')
    if (!form) return
    for (const bar of document.querySelectorAll(
      `[data-ui="bulk-actions"][data-form="${CSS.escape(form)}"]`,
    )) {
      if (checked.length) bar.setAttribute('data-has-selection', 'true')
      else bar.removeAttribute('data-has-selection')
      for (const count of bar.querySelectorAll('[data-ui="bulk-count"]'))
        count.textContent = String(checked.length)
      for (const button of bar.querySelectorAll('[data-ui="bulk-action-list"] button')) {
        if (button instanceof HTMLButtonElement && button.dataset.kvAlwaysDisabled !== 'true')
          button.disabled = checked.length === 0
      }
    }
  }
  /** @param {Event} event */
  const onSelectionChange = (event) => {
    const target = event.target
    if (!(target instanceof HTMLInputElement)) return
    const hook = target.getAttribute('data-ui')
    if (hook !== 'select-all' && hook !== 'row-select') return
    const table = target.closest('[data-ui="table"]')
    if (!table) return
    if (hook === 'select-all') {
      for (const input of table.querySelectorAll('[data-ui="row-select"]')) {
        if (input instanceof HTMLInputElement && !input.disabled) input.checked = target.checked
      }
    }
    syncSelection(table)
  }
  for (const table of root.querySelectorAll('[data-ui="table"]')) {
    if (table.querySelector('[data-ui="row-select"]')) syncSelection(table)
  }
  document.addEventListener('change', onSelectionChange)

  /** @param {MouseEvent} event */
  const onDocumentClick = (event) => {
    const target = event.target
    if (!(target instanceof Node)) return
    const toggle = target instanceof Element ? target.closest('[data-ui="navigation-toggle"]') : null
    if (toggle instanceof HTMLElement && navigationMedia.matches) {
      const navigation = navigations.find(
        (entry) =>
          entry.querySelector('[data-ui="navigation-drawer"]')?.id === toggle.getAttribute('aria-controls'),
      )
      if (navigation) {
        if (navigation.open) closeNavigation(navigation)
        else {
          navigation.open = true
          syncNavigation(navigation)
        }
      }
      return
    }
    for (const menu of root.querySelectorAll('[data-ui="menu"][open]')) {
      if (!(menu instanceof HTMLDetailsElement)) continue
      const item = target instanceof Element ? target.closest('[data-ui="menu-item"]') : null
      if (!menu.contains(target) || (item && item.getAttribute('aria-disabled') !== 'true')) menu.open = false
    }
    // Every other popup built on <details> closes when the reader clicks outside it.
    for (const popup of root.querySelectorAll(DISMISSIBLE_POPUPS_OPEN)) {
      if (popup instanceof HTMLDetailsElement && !popup.contains(target)) popup.open = false
    }
  }
  document.addEventListener('keydown', onKeydown)
  document.addEventListener('click', onDocumentClick)
  const cleanupHelp = attachHelpDrawers(root)
  const cleanupDiagrams = attachMermaidDiagrams(root)

  return () => {
    cleanupHelp()
    cleanupNavigationTools()
    cleanupDiagrams()
    restoreAppRails()
    cleanupGlobalSearch()
    cleanupApplyOnInput()
    cleanupNumberEntry()
    cleanupPrimitives()
    cleanupDatePickers()
    document.removeEventListener('keydown', onKeydown)
    document.removeEventListener('click', onDocumentClick)
    document.removeEventListener('change', onSelectionChange)
    root.removeEventListener('toggle', onMenuToggle, true)
    window.removeEventListener('resize', onMenuResize)
    for (const [menu, observer] of menuObservers) {
      observer.disconnect()
      const panel = menu.querySelector(':scope > [data-ui="menu-panel"]')
      if (panel instanceof HTMLElement)
        for (const property of menuProperties) panel.style.removeProperty(property)
    }
    menuObservers.clear()
    navigationMedia.removeEventListener('change', onNavigationMediaChange)
    for (const cleanup of navigationCleanups) cleanup()
    if (priorNavigationOpen === undefined) delete navigationRoot.dataset.kvNavigationOpen
    else navigationRoot.dataset.kvNavigationOpen = priorNavigationOpen
    if (ownsShellLock && appShell instanceof HTMLElement) appShell.inert = priorInert
    if (modal instanceof HTMLElement && activeBeforeOpen?.isConnected) activeBeforeOpen.focus()
  }
}

window["ATLAS_DESIGN_SYSTEM"] = window["ATLAS_DESIGN_SYSTEM"] || {};
window["ATLAS_DESIGN_SYSTEM"]["attach"] = attachDesignSystemInteractions;
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', () => attachDesignSystemInteractions(), { once: true })
} else {
  attachDesignSystemInteractions()
}

// @ts-nocheck Generated by tools/build-design-system-atlas-runtime.mjs — do not edit.
"use strict";(()=>{var Vt=null,Ka=0,St=new Set,Nn=!1,di=0;function Le(e){let t=e,n=new Set,o=(()=>(Vt&&(n.add(Vt),Vt.deps.add(n)),t));return o.set=i=>{let s=typeof i=="function"?i(t):i;if(Object.is(s,t))return t;t=s;for(let m of n)St.add(m);return Ka===0&&ci(),t},o.peek=()=>t,o}function ci(){if(Nn||Ka>0)return;Nn=!0;let e=++di,t=[];try{for(;St.size;){let n;for(let o of St)if(o.computed){n=o;break}if(n??=St.values().next().value,!n)break;if(St.delete(n),n.epoch!==e&&(n.epoch=e,n.runs=0),++n.runs>100){n.runs===101&&t.push(new Error("signal cycle: an effect ran 100 times in one flush; it writes a signal it also reads"));continue}try{Pa(n)}catch(o){t.push(o)}}}finally{Nn=!1}if(t.length===1)throw t[0];if(t.length>1)throw new AggregateError(t,`${t.length} effects failed in one flush`)}function Pa(e){if(e.disposed)return;let t=e.cleanup;e.cleanup=null;for(let o of e.deps)o.delete(e);e.deps.clear(),t?.();let n=Vt;Vt=e;try{let o=e.fn();typeof o=="function"&&(e.cleanup=o)}finally{Vt=n}}function ui(e,t){let n={fn:e,deps:new Set,disposed:!1,cleanup:null,computed:t,epoch:0,runs:0};try{Pa(n)}catch(o){n.disposed=!0,St.delete(n);for(let i of n.deps)i.delete(n);throw n.deps.clear(),n.cleanup?.(),n.cleanup=null,o}return()=>{n.disposed=!0,St.delete(n);for(let o of n.deps)o.delete(n);n.deps.clear(),n.cleanup?.(),n.cleanup=null}}function kt(e){return ui(e,!1)}var Na=new RegExp("\uFFFC(\\d+)\uFFFC"),Tt=class extends Error{code="E_TEMPLATE_HOLE";hint;constructor(t,n){super(t),this.name="TemplateError",this.hint=n}},Fa=e=>e.includes("\uFFFC"),gi=new Set(["area","base","br","col","embed","hr","img","input","link","meta","source","track","wbr"]);function pi(e){let t=e.reduce((c,y,b)=>c+y+(b<e.length-1?"\uFFFC"+b+"\uFFFC":""),""),n=0,o={type:"root",children:[]},i=[o],s=()=>i[i.length-1],m=c=>{let y=c;for(;;){if(!y)return;let b=Na.exec(y);if(!b){s().children.push({type:"text",value:y});return}b.index>0&&s().children.push({type:"text",value:y.slice(0,b.index)}),s().children.push({type:"hole",index:Number(b[1])}),y=y.slice(b.index+b[0].length)}};for(;n<t.length;){let c=t.indexOf("<",n);if(c===-1){m(t.slice(n));break}if(m(t.slice(n,c)),t.startsWith("<!--",c)){n=t.indexOf("-->",c)+3;continue}if(t[c+1]==="/"){let C=t.indexOf(">",c);i.length>1&&i.pop(),n=C+1;continue}let y=c+1;for(;y<t.length&&/[^\s/>]/.test(t[y]);)y++;let b=t.slice(c+1,y);if(Fa(b))throw new Tt("an element name cannot be a hole","the tag decides the shape of the tree, which is fixed per call site \u2014 branch with when() and write each tag out");let w=[];for(;;){for(;y<t.length&&/\s/.test(t[y]);)y++;if(t[y]===">"||t.startsWith("/>",y))break;let C=y;for(;C<t.length&&/[^\s=/>]/.test(t[C]);)C++;let K=t.slice(y,C);if(Fa(K))throw new Tt("an attribute name cannot be a hole","a hole fills an attribute value; to add an attribute conditionally, pass null as its value");if(!K){y=C+1;continue}for(;C<t.length&&/\s/.test(t[C]);)C++;if(t[C]!=="="){w.push({name:K,value:""}),y=C;continue}for(C++;C<t.length&&/\s/.test(t[C]);)C++;let f,V=t[C];if(V==='"'||V==="'"){let j=t.indexOf(V,C+1);f=t.slice(C+1,j),C=j+1}else{let j=C;for(;j<t.length&&!/[\s/>]/.test(t[j]);)j++;f=t.slice(C,j),C=j}let A=Na.exec(f);if(A&&A[0]!==f)throw new Tt(`an attribute takes a whole value or none: "${K}" mixes a hole with static text`,`write ${K}=\${...} and build the whole value in the expression, not "text \${...}"`);if(A&&/^srcdoc$/i.test(K))throw new Tt("srcdoc cannot be bound: its value is a whole document, rendered with the page origin","point the iframe at a route with src=${...} and a sandbox attribute, or write srcdoc out in the template");if(A&&/^on[a-z]/i.test(K))throw new Tt(`"${K}" cannot be bound: a string inline handler is script`,`bind a function with on:${K.slice(2).toLowerCase()}=\${...} instead`);A?w.push({name:K,hole:Number(A[1])}):w.push({name:K,value:f}),y=C}let E=t.startsWith("/>",y),k=t.indexOf(">",y),M={type:"el",tag:b,attrs:w,children:[]};s().children.push(M),!E&&!gi.has(b)&&i.push(M),n=k+1}return o}var Ha=new WeakMap;function vn(e){let t=Ha.get(e);return t||(t=pi(e),Ha.set(e,t)),t}var wn="http://www.w3.org/2000/svg",bi=/[&<>"']/,Ct=e=>{let t=String(e);return bi.test(t)?yi(t):t};function yi(e){let t="",n=0;for(let o=0;o<e.length;o++){let i;switch(e.charCodeAt(o)){case 38:i="&amp;";break;case 60:i="&lt;";break;case 62:i="&gt;";break;case 34:i="&quot;";break;case 39:i="&#39;";break;default:continue}o>n&&(t+=e.slice(n,o)),t+=i,n=o+1}return n<e.length?t+e.slice(n):t}function Hn(e=globalThis.document){let t=n=>n;return{ops:null,createElement:(n,o)=>o&&e.createElementNS?e.createElementNS(o,n):e.createElement(n),createText:n=>e.createTextNode(String(n)),setText:(n,o)=>{t(n).data=String(o)},setAttribute:(n,o,i)=>{i==null||i===!1?t(n).removeAttribute(o):t(n).setAttribute(o,String(i))},insert:(n,o,i=null)=>{t(n).insertBefore(o,i)},insertMarkup:(n,o,i=null)=>{let s;if(e.parseHTML)s=e.parseHTML(o);else{let c=e.createElement("template");c.innerHTML=o,s=c.content}let m=[...s.childNodes];for(let c of m)t(n).insertBefore(c,i);return m},move:(n,o,i=null)=>{t(n).insertBefore(o,i)},remove:n=>{t(n).remove()},listen:(n,o,i)=>{let s=n;return s.addEventListener(o,i),()=>s.removeEventListener(o,i)}}}var vi=new Set(["href","src","action","formaction","xlink:href","poster","data","cite"]),Da=e=>vi.has(e.toLowerCase()),wi=/^(?:javascript|vbscript):/,ki=/^data:(?:image|video|audio)\//,Va=e=>{if(e==null||typeof e=="boolean")return e;let t=String(e),n="";for(let o=0;o<t.length&&n.length<16;o++){let i=t.charCodeAt(o);i>32&&i!==127&&(n+=t[o])}return n=n.toLowerCase(),wi.test(n)||n.startsWith("data:")&&!ki.test(n)?`unsafe:${t}`:t},kn=(e,t)=>Da(e)?Va(t):t;var Ot="ket-island",Lt="data-ket-island",xn=`${Ot},div[${Lt}]`;var jt="k",Qt="k[",xi=new Set(["area","base","br","col","embed","hr","img","input","link","meta","source","track","wbr"]);function Bn(e){return jn(e,!1)}var en=e=>typeof e=="object"&&e!==null&&typeof e.html=="string"&&Ri in e,Ri=Symbol.for("ket.markup");function Mi(e,t){if(typeof e=="string")return Ct(e);if(en(e))return e.html;if(Bt(e))return jn(e,t);if(qt(e)){let n=e,o="";for(let i=0;i<n.items.length;i++)o+=jn(n.render(n.items[i],i),t);return o}return e==null||e===!1?"":Ct(e)}var qn=new Set(["title","textarea","style","script","noscript"]),ja={hydratable:new WeakMap,static:new WeakMap},Dn=e=>e.name===Lt;function Vn(e,t,n,o){let i=[],s="",m=()=>{s&&i.push(s),s=""},c=(y,b,w,E)=>{if(y.type==="text"){s+=y.value;return}if(y.type==="hole"){let f=!b&&w;f&&(s+=`<!--${Qt}-->`),m(),i.push({hole:y.index,hydrate:f}),f&&(s+=`<!--${jt}-->`);return}let k=y,M=k.tag==="div"&&k.attrs.some(f=>Dn(f)&&f.hole==null);if(E===null&&!w&&!M&&k.tag==="div"&&k.attrs.some(Dn)){m(),i.push({element:k,raw:b,hydrate:w,hosted:null,plain:null});return}s+=`<${k.tag}`;for(let f of k.attrs)if(!f.name.startsWith(tn)){if(f.hole!=null){m(),i.push({attr:` ${f.name}="`,name:f.name,hole:f.hole});continue}f.value!=null&&(s+=` ${f.name}="${Ct(f.value)}"`)}if(s+=">",xi.has(k.tag))return;let C=b||qn.has(k.tag),K=w||k.tag===Ot||(E??M);for(let f of k.children)c(f,C,K,null);s+=`</${k.tag}>`};for(let y of e)c(y,t,n,o);return m(),i}function Ba(e,t){let n="";for(let o=0;o<e.length;o++){let i=e[o];if(typeof i=="string")n+=i;else if("attr"in i){let s=kn(i.name,t[i.hole]);s!=null&&s!==!1&&(n+=`${i.attr}${Ct(s)}"`)}else if("hole"in i)n+=Mi(t[i.hole],i.hydrate);else{let m=i.element.attrs.some(c=>{if(!Dn(c))return!1;let y=c.hole==null?c.value:t[c.hole];return y!=null&&y!==!1})?i.hosted??=Vn([i.element],i.raw,i.hydrate,!0):i.plain??=Vn([i.element],i.raw,i.hydrate,!1);n+=Ba(m,t)}}return n}function jn(e,t){let n=vn(e.strings),o=t?ja.hydratable:ja.static,i=o.get(n);return i||(i=Vn(n.children,!1,t,null),o.set(n,i)),Ba(i,e.values)}var Ei={tbody:"table",thead:"table",tfoot:"table",tr:"tbody",html:"(document)",head:"html",body:"html"},nt=class extends Error{code="E_HYDRATION_MISMATCH";hint;constructor(t,n,o){super(`hydration mismatch at ${t}: expected ${n}, found ${o}`);let i=Object.keys(Ei).find(s=>o.includes(`<${s}>`));this.hint=i?`the HTML parser inserted <${i}> on its own \u2014 write it in the template so the server and the browser agree`:"the markup does not match the template that rendered it; fall back to a clean client render"}};var qa=Symbol("ket.result"),za=Symbol("ket.each");var Mn=(e,t)=>({[qa]:!0,strings:e,values:t}),Bt=e=>!!e?.[qa];function we(e,t,n){return{[za]:!0,items:[...e],keyOf:t,render:n}}var qt=e=>!!e?.[za];var tn="on:",_a=e=>e.startsWith(tn);function Ja(e,t,n,o){let i={fn:typeof o=="function"?o:null};return{event:!0,detach:e.listen(t,n.slice(tn.length),m=>i.fn?.(m)),box:i}}var zt=class{attr=!1;host;parent;anchor;kind=null;node=null;child=null;keyed=null;keys=[];markupNodes=[];markupHtml="";namespace;constructor(t,n,o,i){this.host=t,this.parent=n,this.anchor=o,this.namespace=i}clear(){if(this.node&&(this.host.remove(this.node),this.node=null),this.child&&(this.child.remove(),this.child=null),this.keyed){for(let t of this.keyed.values())t.remove();this.keyed=null,this.keys=[]}for(let t of this.markupNodes)this.host.remove(t);this.markupNodes=[],this.markupHtml="",this.kind=null}disposeBehavior(){if(this.child?.dispose(!1),this.keyed)for(let t of this.keyed.values())t.dispose(!1)}collect(t){if(this.kind==="text")this.node&&t.push(this.node);else if(this.kind==="result")this.child?.collect(t);else if(this.kind==="each"&&this.keyed)for(let n of this.keys)this.keyed.get(n)?.collect(t);else this.kind==="markup"&&t.push(...this.markupNodes)}firstNode(){return this.kind==="text"&&this.node?this.node:this.kind==="result"&&this.child?this.child.firstNode()??this.anchor:this.kind==="each"&&this.keyed&&this.keys.length?this.keyed.get(this.keys[0])?.firstNode()??this.anchor:this.kind==="markup"&&this.markupNodes.length?this.markupNodes[0]:this.anchor}commit(t){if(Bt(t)){this.commitResult(t);return}if(qt(t)){this.commitEach(t);return}if(en(t)){this.commitMarkup(t.html);return}this.commitText(t)}commitMarkup(t){this.kind==="markup"&&this.markupHtml===t||(this.clear(),this.kind="markup",this.markupHtml=t,this.markupNodes=this.host.insertMarkup(this.parent,t,this.anchor))}commitText(t){let n=t==null||t===!1?"":String(t);if(this.kind==="text"&&this.node){this.node.text!==n&&this.host.setText(this.node,n);return}this.clear(),this.kind="text",this.node=this.host.createText(n),this.host.insert(this.parent,this.node,this.anchor)}commitResult(t){if(this.kind==="result"&&this.child&&this.child.strings===t.strings){this.child.update(t.values);return}this.clear(),this.kind="result",this.child=new At(this.host,t.strings),this.child.mount(this.parent,this.anchor,this.namespace),this.child.update(t.values)}commitEach(t){this.kind!=="each"&&(this.clear(),this.kind="each",this.keyed=new Map,this.keys=[]);let n=this.keyed,o=this.keys,i=t.items.length,s=new Array(i),m=new Array(i),c=o.length===i;for(let k=0;k<i;k++){let M=t.keyOf(t.items[k],k);s[k]=M,m[k]=t.render(t.items[k],k),c&&o[k]!==M&&(c=!1)}if(c){for(let k=0;k<i;k++){let M=n.get(s[k]),C=m[k];if(M&&M.strings===C.strings){M.update(C.values);continue}c=!1;break}if(c){for(let k=0;k<i;k++)n.get(s[k]).pos=k;this.keys=s;return}}Si(s);let y=new Array(i),b=0;for(let k=0;k<i;k++){let M=n.get(s[k]);M&&M.strings===m[k].strings?(y[k]=M.pos,b++):y[k]=-1}if(b!==n.size){let k=new Set(s);for(let[M,C]of n)k.has(M)||(C.remove(),n.delete(M))}let w=Ti(y),E=this.anchor;for(let k=s.length-1;k>=0;k--){let M=s[k],C=m[k],K=n.get(M);!!K&&K.strings===C.strings&&K?(K.update(C.values),w[k]!==1&&K.moveBefore(this.parent,E)):(K&&K.remove(),K=new At(this.host,C.strings),K.mount(this.parent,E,this.namespace),K.update(C.values),n.set(M,K)),K.pos=k,E=K.firstNode()??E}this.keys=s}},an=class extends Error{code="E_DUPLICATE_KEY";hint;constructor(t){super(`each() received the key ${JSON.stringify(t)??String(t)} twice`),this.name="DuplicateKeyError",this.hint="keyOf must return a distinct value per item \u2014 an id rather than a field several items share"}};function Si(e){let t=new Set;for(let n of e){if(t.has(n))throw new an(n);t.add(n)}}function Ti(e){let t=[],n=new Array(e.length).fill(-1),o=[];for(let m=0;m<e.length;m++){let c=e[m];if(c<0)continue;let y=0,b=t.length;for(;y<b;){let w=y+b>>1;e[t[w]]<c?y=w+1:b=w}y>0&&(n[m]=t[y-1]),t[y]=m,o[y]=c}let i=new Uint8Array(e.length),s=t.length?t[t.length-1]:-1;for(;s>=0;)i[s]=1,s=n[s];return i}var At=class{host;strings;tpl;parts=[];roots=[];values=[];pos=-1;constructor(t,n){this.host=t,this.strings=n,this.tpl=vn(n)}mount(t,n,o){for(let i of this.tpl.children)this.build(i,t,o,t,n)}build(t,n,o,i,s){let m=n===i;if(t.type==="text"){let w=this.host.createText(t.value);this.host.insert(n,w,m?s:null),m&&this.roots.push(w);return}if(t.type==="hole"){let w=this.host.createText("");this.host.insert(n,w,m?s:null),m&&this.roots.push(w),this.parts[t.index]=new zt(this.host,n,w,o);return}let c=t.tag,y=c==="svg"?wn:c==="foreignObject"?void 0:o,b=this.host.createElement(c,y);for(let w of t.attrs){if(w.hole==null){this.host.setAttribute(b,w.name,w.value??"");continue}this.parts[w.hole]=_a(w.name)?Ja(this.host,b,w.name,void 0):{attr:!0,node:b,name:w.name,last:void 0}}this.host.insert(n,b,m?s:null),m&&this.roots.push(b);for(let w of t.children)this.build(w,b,y,i,s)}update(t){for(let n=0;n<t.length;n++){let o=this.parts[n];if(!o)continue;let i=t[n];!("event"in o)&&!o.attr&&Object.is(this.values[n],i)&&!Bt(i)&&!qt(i)||this.apply(o,i)}this.values=t}apply(t,n){if("event"in t){t.box.fn=typeof n=="function"?n:null;return}if(t.attr){Object.is(t.last,n)||(this.host.setAttribute(t.node,t.name,kn(t.name,n)),t.last=n);return}t.commit(n)}firstNode(){let t=this.roots[0];if(!t)return null;for(let n of this.parts)if(n instanceof zt&&n.anchor===t)return n.firstNode();return t}nextSibling(){let t=this.roots[this.roots.length-1];if(!t)return null;let n=t.nextSibling;if(n!==void 0)return n??null;if(!t.parent)return null;let o=t.parent.children??[];return o[o.indexOf(t)+1]??null}collect(t){let n=null;for(let o of this.parts)o instanceof zt&&(n??=new Map).set(o.anchor,o);for(let o of this.roots)n?.get(o)?.collect(t),t.push(o)}moveBefore(t,n){let o=[];this.collect(o);for(let i of o)this.host.move(t,i,n)}remove(){this.dispose(!0)}dispose(t){for(let n of this.parts)if(n){if("event"in n){n.detach();continue}n.attr||(t?n.clear():n.disposeBehavior())}if(t){for(let n of this.roots)this.host.remove(n);this.roots=[]}}};function Jn(e,t){let n=null;return{render(o){(!n||n.strings!==o.strings)&&(n?.remove(),n=new At(e,o.strings),n.mount(t,null)),n.update(o.values)},dispose(o={}){n?.dispose(o.remove===!0),n=null}}}var Ci=1,zn=3,Rn=8,nn=e=>e?e.nodeType===Rn?`comment "${e.data??""}"`:e.nodeType===zn?`text "${(e.data??"").slice(0,20)}"`:`<${e.nodeName.toLowerCase()}>`:"nothing";function Xa(e,t=[]){for(let n of e)if(n.type==="hole")t.push(n.index);else if(n.type==="el"){for(let o of n.attrs)o.hole!=null&&t.push(o.hole);Xa(n.children,t)}return t}function _n(e,t,n,o,i,s){let m=new At(e,t),c=i,y=(w,E)=>{if(en(w)){for(E.kind="markup";c&&!(c.nodeType===Rn&&c.data===jt);)E.markupNodes.push(c),c=c.nextSibling;E.markupHtml=w.html;return}if(Bt(w)){let k=_n(e,w.strings,w.values,E.parent,c,E.namespace);E.kind="result",E.child=k.instance,c=k.cursor;return}if(qt(w)){E.kind="each",E.keyed=new Map,E.keys=[];for(let k=0;k<w.items.length;k++){let M=w.items[k],C=w.render(M,k),K=_n(e,C.strings,C.values,E.parent,c,E.namespace),f=w.keyOf(M,k);if(E.keyed.has(f))throw new an(f);K.instance.pos=k,E.keyed.set(f,K.instance),E.keys.push(f),c=K.cursor}return}if(w==null||w===!1||w===""){E.kind=null;return}if(!c||c.nodeType!==zn)throw new nt("a text hole","a text node",nn(c));E.kind="text",E.node=c,c=c.nextSibling},b=(w,E,k)=>{let M=E===o;if(w.type==="text"){if(!c||c.nodeType!==zn)throw new nt("static text",JSON.stringify(w.value.slice(0,20)),nn(c));M&&m.roots.push(c),c=c.nextSibling;return}if(w.type==="hole"){if(!c||c.nodeType!==Rn||c.data!==Qt)throw new nt("the start of a hole",`a <!--${Qt}--> marker`,nn(c));let A=c;M&&m.roots.push(A),c=c.nextSibling;let j=new zt(e,E,null,k);if(y(n[w.index],j),!c||c.nodeType!==Rn||c.data!==jt)throw new nt("a hole",`a <!--${jt}--> marker`,nn(c));j.anchor=c,M&&m.roots.push(c),m.parts[w.index]=j,c=c.nextSibling;return}let C=w;if(!c||c.nodeType!==Ci||c.nodeName.toLowerCase()!==C.tag.toLowerCase())throw new nt("an element",`<${C.tag}>`,nn(c));let K=c;M&&m.roots.push(K);for(let A of C.attrs)A.hole!=null&&(m.parts[A.hole]=_a(A.name)?Ja(e,K,A.name,n[A.hole]):{attr:!0,node:K,name:A.name,last:n[A.hole]});let f=K.nextSibling,V=C.tag==="svg"?wn:C.tag==="foreignObject"?void 0:k;if(qn.has(C.tag.toLowerCase())){for(let A=K.firstChild;A;A=K.firstChild)e.remove(A);for(let A of C.children)m.build(A,K,V,null,null);for(let A of Xa(C.children)){let j=m.parts[A];j&&m.apply(j,n[A])}c=f;return}c=K.firstChild;for(let A of C.children)b(A,K,V);c=f};for(let w of m.tpl.children)b(w,o,s);return m.values=n,{instance:m,cursor:c}}function Xn(e,t,n){let{instance:o}=_n(e,n.strings,n.values,t,t.firstChild),i=o;return{render(s){(!i||i.strings!==s.strings)&&(i?.remove(),i=new At(e,s.strings),i.mount(t,null)),i.update(s.values)},dispose(s={}){i?.dispose(s.remove===!0),i=null}}}function Oi(e,t){let n=kt(()=>{e.render(t())});return{dispose:()=>{n(),e.dispose()},refresh:()=>e.render(t())}}function En(e,t,n){return Oi(Jn(e,t),n)}function Un(e,t,n){let o=null,i=kt(()=>{let s=n();o?o.render(s):o=Xn(e,t,s)});return{dispose:()=>{i(),o?.dispose()},refresh:()=>o?.render(n())}}var _t=class extends Error{code;hint;constructor(t){super(t.message),this.name="IslandError",this.code=t.code,this.hint=t.hint??null}};var Ai=e=>typeof e=="function"?{view:e}:e,Gi=(e,t)=>{let n=new WeakSet,o=i=>{if(i==null||typeof i=="string"||typeof i=="boolean")return i;if(typeof i=="number"){if(!Number.isFinite(i))throw new TypeError("non-finite number");return i}if(typeof i!="object")throw new TypeError(typeof i);if(i instanceof Date){if(Number.isNaN(i.getTime()))throw new TypeError("invalid date");return i.toJSON()}if(n.has(i))throw new TypeError("cyclic value");n.add(i);let s;if(Array.isArray(i))s=i.map(o);else{let m=Object.getPrototypeOf(i);if(m!==null&&m!==Object.prototype)throw new TypeError("non-plain object");let c=Object.create(null);for(let y of Object.keys(i).sort())c[y]=o(i[y]);s=c}return n.delete(i),s};try{let i=o(t),s=JSON.stringify(i),m=JSON.parse(s);if(typeof m!="object"||m===null||Array.isArray(m))throw new TypeError("not an object");return{raw:s,revived:m}}catch{throw new _t({code:"E_ISLAND_PROPS",message:`island "${e}" received props that are not JSON-serializable`,hint:"island props cross the server/browser boundary; pass plain JSON data only"})}};var rn=e=>{let t=[],n=e.nodeName??e.tagName;return(n?.toLowerCase()===Ot||n?.toLowerCase()==="div"&&e.getAttribute(Lt)!==null)&&t.push(e),t.push(...e.querySelectorAll(xn)),t},Wn=e=>[...e.childNodes??e.children??[]],Zn=(e,t)=>{let n=t.getAttribute("data-props");try{let o=n?JSON.parse(n):{};if(typeof o!="object"||o===null||Array.isArray(o))throw new TypeError;let i=Gi(e,o);return{props:i.revived,raw:i.raw}}catch{throw new _t({code:"E_ISLAND_PROPS",message:`island "${e}" has unreadable props`})}},Yn=e=>{let t=e.getAttribute("data-island");if(!t)return null;let n=Zn(t,e),o=e.getAttribute("data-key")??n.raw;return e.setAttribute?.("data-key",o),{name:t,key:o,id:JSON.stringify([t,o])}},Ua=(e,t)=>{let n=new Map,o=new Set;for(let i of e){let s=Yn(i);s&&(n.has(s.id)?(o.add(s.id),n.delete(s.id),t?.(s)):o.has(s.id)||n.set(s.id,i))}return n};function Qn(e,t,n={}){let o=new WeakMap,i=y=>{let b=o.get(y);b&&(o.delete(y),b.live.dispose())},s=(y,b)=>{let w=[],E=new Set;for(let k of rn(y)){if(E.has(k))continue;let M=o.get(k);if(M){w.push(M.live);continue}let C=Yn(k);if(!C)continue;let K=t[C.name];if(!K){if(n.strict===!1)continue;throw new _t({code:"E_UNKNOWN_ISLAND",message:`the page places island "${C.name}", which no composed module provides`,hint:`registered islands: ${Object.keys(t).join(", ")||"(none)"}`})}let f=Zn(C.name,k),V=Ai(K(f.props)),A=new AbortController,j=null,G=null;try{if(!b)j=En(e,k,V.view);else try{j=Un(e,k,V.view)}catch(ne){if(!(ne instanceof nt)||n.hydrationMismatch==="throw")throw ne;G=ne;for(let Q of rn(k))Q!==k&&E.add(Q);for(let Q of Wn(k))e.remove(Q);j=En(e,k,V.view)}V.mount?.({root:k,lifetime:A.signal})}catch(ne){throw A.abort(),j?.dispose(),V.dispose?.(),ne}let Y=!1,D={name:C.name,key:C.key,props:f.props,element:k,dispose:()=>{Y||(Y=!0,A.abort(),j?.dispose(),V.dispose?.())}};if(o.set(k,{live:D,controller:V,rawProps:f.raw}),w.push(D),!!G){n.onHydrationMismatch?n.onHydrationMismatch(G,D):console.error(`island "${C.name}" did not match its server markup and was rendered on the client`,G);for(let ne of s(k,!0))ne!==D&&w.push(ne)}}return w},m=y=>s(y,!0);return{hydrate:m,mount:y=>s(y,!1),reconcile:(y,b)=>{let w=rn(y),E=rn(b),k=new Map,M=V=>k.set(V.id,V),C=Ua(w,M),K=Ua(E,M);for(let V of k.values())console.warn(`island "${V.name}" has duplicate key ${V.key}; remounting ambiguous instances`);let f=new Set;for(let[V,A]of K){let j=C.get(V),G=j?o.get(j):void 0;if(!j||!G)continue;let Y=Yn(A),D=Zn(Y.name,A);if(G.rawProps!==D.raw){if(!G.controller.update)continue;G.controller.update(D.props),G.rawProps=D.raw,G.live.props=D.props,j.setAttribute?.("data-props",D.raw),j.setAttribute?.("data-key",Y.key)}let ne=A.parentNode??A.parent;ne&&(e.insert(ne,j,A),e.remove(A),f.add(j))}for(let V of w)f.has(V)||i(V);for(let V of Wn(y))e.remove(V);for(let V of Wn(b))e.insert(y,V,null);return m(y)},dispose:y=>{for(let b of rn(y))i(b)}}}var ta={n:1n,d:1n},na={n:0n,d:1n},Jt=(e,t)=>({n:e.n*t.n,d:e.d*t.d}),Sn=(e,t)=>({n:e.n*t.d+t.n*e.d,d:e.d*t.d}),aa=e=>({n:-e.n,d:e.d}),ea=(e,t)=>t.n===0n?null:{n:e.n*t.d*(t.n<0n?-1n:1n),d:e.d*(t.n<0n?-t.n:t.n)},ra=e=>{if(typeof e!="string"&&typeof e!="number")return null;let t=String(e).trim(),n=/^([+-]?)(\d+)(?:\.(\d*))?(?:e([+-]?\d+))?$/iu.exec(t);if(!n||t.length>90)return null;let o=Number(n[4]??0)-(n[3]?.length??0);if(!Number.isInteger(o)||Math.abs(o)>40)return null;let i=BigInt(n[2]+(n[3]??""))*(n[1]==="-"?-1n:1n);return o>=0?{n:i*10n**BigInt(o),d:1n}:{n:i,d:10n**BigInt(-o)}},Xt=(e,t)=>{let n=ta;for(let o of e){let i=ra(t[o]);if(!i)return null;n=Jt(n,i)}return n},ia=(e,t)=>{let n=ra(e.percentOff?t[e.percentOff]??0:0);return!n||n.n<0n||n.n>100n*n.d?null:Sn(ta,aa(Jt(n,{n:1n,d:100n})))},Ii=e=>{if(!e)return null;let t=Number(e.n)/Number(e.d);return Number.isFinite(t)&&Math.abs(t)<=Number.MAX_SAFE_INTEGER?Math.round(t*1e6)/1e6:null},Wa=(e,t)=>{if("difference"in e){let[c,y]=e.difference.map(E=>t[E]),b=c==null||c===""?Number.NaN:Number(c),w=y==null||y===""?Number.NaN:Number(y);return Number.isFinite(b)&&Number.isFinite(w)?Math.round((b-w)*1e6)/1e6:null}if(!e.less&&!e.as&&!e.solve){let c=1;for(let b of e.multiply){let w=Number(t[b]??0);c*=Number.isFinite(w)?w:0}let y=e.percentOff?Number(t[e.percentOff]??0):0;return Math.round(c*(1-(Number.isFinite(y)?y:0)/100)*1e6)/1e6}let n=Xt(e.multiply,t),o=ia(e,t),i=e.less?Xt(e.less,t):na;if(!n||!o||!i)return null;let s=Jt(n,o),m=Sn(s,aa(i));return Ii(e.as==="percent"?ea(Jt(m,{n:100n,d:1n}),s):m)},oa=(e,t)=>{if(!("multiply"in e)||!e.solve||e.multiply.filter(s=>s===e.solve.field).length!==1)return!1;let n=Xt(e.multiply.filter(s=>s!==e.solve.field),t),o=ia(e,t),i=e.less?Xt(e.less,t):na;return!!n&&n.n>0n&&!!o&&o.n>0n&&!!i&&i.n>=0n},Za=(e,t,n,o)=>{if(!("multiply"in e)||!e.solve||!oa(e,t))return t;let i=ra(n),s=e.less?Xt(e.less,t):na;if(!i)return t;let m=Sn(i,s);if(e.as==="percent"&&(i.n>=100n*i.d||(m=ea(s,Sn(ta,aa(Jt(i,{n:1n,d:100n})))),!m||m.n===0n))||!m||m.n<0n)return t;let c=Xt(e.multiply.filter(K=>K!==e.solve.field),t),y=ea(m,Jt(c,ia(e,t))),b=e.solve.digits??6;if(!Number.isInteger(b)||b<0||b>6)return t;let w=10n**BigInt(b),E=y.n*w,k=E/y.d+(E%y.d*2n>=y.d?1n:0n);if(k>BigInt(Number.MAX_SAFE_INTEGER))return t;let M=k.toString().padStart(b+1,"0"),C=b?`${M.slice(0,-b)}.${M.slice(-b)}`:M;return{...t,[e.solve.field]:o?C:Number(C)}};var Ya=new Set(["area","base","br","col","embed","hr","img","input","link","meta","source","track","wbr"]),Ki=/^[a-z][a-z0-9-]*$/,Pi=/^[A-Za-z_:][A-Za-z0-9:._-]*$/,Qa=-1,er=-2,la=new Map,tr=new Map,ar=e=>{let t=[...e];return Object.defineProperty(t,"raw",{value:Object.freeze([...e]),enumerable:!1}),Object.freeze(t)},nr=e=>{let t=tr.get(e);if(t)return t;let n=ar(Array.from({length:e+1},()=>""));return tr.set(e,n),n},sa=e=>{if(!Array.isArray(e))return Mn(nr(1),[Tn(e)]);let t=new Array(e.length);for(let n=0;n<e.length;n++)t[n]=Tn(e[n]);return Mn(nr(t.length),t)},Tn=e=>Array.isArray(e)?sa(e):e,$i=e=>e.replace(/[A-Z]/g,t=>`-${t.toLowerCase()}`),Ni=e=>!e||typeof e!="object"||Array.isArray(e)?e:Object.entries(e).filter(([,t])=>t!=null&&t!==!1).map(([t,n])=>`${$i(t)}:${String(n)}`).join(";"),Fi=e=>e==="className"?"class":e==="htmlFor"?"for":/^on[A-Z]/.test(e)?`on:${e.slice(2).toLowerCase()}`:e,rr=e=>e==null||e===!1;function Hi(e,t,n,o){if(!Ki.test(e))throw new TypeError(`invalid JSX element name "${e}"`);if("dangerouslySetInnerHTML"in t)throw new TypeError("Ket JSX has no dangerouslySetInnerHTML; pass trusted compiler output through trustedMarkup()");if("ref"in t)throw new TypeError("Ket JSX does not expose mutable element refs");let i=n.map(Fi),s=new Set;for(let c of i){if(!Pi.test(c))throw new TypeError(`invalid JSX attribute name "${c}"`);if(s.has(c))throw new TypeError(`JSX attribute "${c}" was provided more than once`);s.add(c)}if(Ya.has(e)&&!rr(t.children))throw new TypeError(`<${e}> is a void element and cannot have children`);let m=[`<${e}`];for(let c of i)m[m.length-1]+=` ${c}=`,m.push("");m[m.length-1]+=">";for(let c=0;c<o;c++)m.push("");return Ya.has(e)||(m[m.length-1]+=`</${e}>`),{strings:ar(m),style:n.indexOf("style")}}function ir(e,t,n){let o=Object.keys(t),i=[],s=null,m=la.get(e);for(let w=0;w<o.length;w++){let E=o[w];if(E==="children"||E==="key"){s||(s=o.slice(0,w));continue}s?.push(E),i.push(t[E]),m=m?.next?.get(E)}let c=t.children,y=Qa;if(!rr(c))if(n&&Array.isArray(c)){y=c.length;for(let w=0;w<c.length;w++)i.push(Tn(c[w]))}else y=er,i.push(Tn(c));let b=m?.shapes?.get(y);if(!b){let w=s??o;b=Hi(e,t,w,y===Qa?0:y===er?1:y);let E=la.get(e);E||la.set(e,E={next:null,shapes:null});for(let k of w){E.next??=new Map;let M=E.next.get(k);M||E.next.set(k,M={next:null,shapes:null}),E=M}E.shapes??=new Map,E.shapes.set(y,b)}return b.style!==-1&&(i[b.style]=Ni(i[b.style])),Mn(b.strings,i)}function r(e,t,n){let o=t??{};return typeof e=="function"?e(o):ir(e,o,!1)}function R(e,t,n){let o=t??{};return typeof e=="function"?e(o):ir(e,o,!0)}function P(e){return e.children===void 0||e.children===null||e.children===!1?sa([]):sa(e.children)}var or=e=>r("div",{"data-ui":"skeleton",role:e.decorative?void 0:"status","aria-label":e.decorative?null:e.label,"aria-hidden":e.decorative?"true":null,children:we(Array.from({length:Math.max(1,Math.min(20,Math.trunc(Number.isFinite(e.lines)?e.lines??3:3)))}),(t,n)=>n,(t,n)=>r("span",{"data-ui":"skeleton-line","data-line":String(n+1)}))});var da=e=>r("span",{"data-ui":"spinner","data-size":e.size??"default",role:e.decorative?void 0:"status","aria-hidden":e.decorative?"true":null,children:!e.decorative&&r("span",{"data-ui":"visually-hidden",children:e.label})});var Di={"layers-3":()=>R(P,{children:[r("path",{d:"M12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 1.66 0l8.58-3.9a1 1 0 0 0 0-1.83z"}),r("path",{d:"M2 12a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 12"}),r("path",{d:"M2 17a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 17"})]}),"building-2":()=>R(P,{children:[r("path",{d:"M6 22V4a2 2 0 0 1 2-2h8a2 2 0 0 1 2 2v18Z"}),r("path",{d:"M6 12H4a2 2 0 0 0-2 2v6a2 2 0 0 0 2 2h2"}),r("path",{d:"M18 9h2a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2h-2"}),r("path",{d:"M10 6h4"}),r("path",{d:"M10 10h4"}),r("path",{d:"M10 14h4"}),r("path",{d:"M10 18h4"})]}),"layout-grid":()=>R(P,{children:[r("rect",{width:"7",height:"7",x:"3",y:"3",rx:"1"}),r("rect",{width:"7",height:"7",x:"14",y:"3",rx:"1"}),r("rect",{width:"7",height:"7",x:"14",y:"14",rx:"1"}),r("rect",{width:"7",height:"7",x:"3",y:"14",rx:"1"})]}),sun:()=>R(P,{children:[r("circle",{cx:"12",cy:"12",r:"4"}),r("path",{d:"M12 2v2"}),r("path",{d:"M12 20v2"}),r("path",{d:"m4.93 4.93 1.41 1.41"}),r("path",{d:"m17.66 17.66 1.41 1.41"}),r("path",{d:"M2 12h2"}),r("path",{d:"M20 12h2"}),r("path",{d:"m6.34 17.66-1.41 1.41"}),r("path",{d:"m19.07 4.93-1.41 1.41"})]}),moon:()=>r(P,{children:r("path",{d:"M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z"})}),megaphone:()=>R(P,{children:[r("path",{d:"m3 11 18-5v12L3 14v-3z"}),r("path",{d:"M7 14v7"})]}),"layout-dashboard":()=>R(P,{children:[r("rect",{width:"7",height:"9",x:"3",y:"3",rx:"1"}),r("rect",{width:"7",height:"5",x:"14",y:"3",rx:"1"}),r("rect",{width:"7",height:"9",x:"14",y:"12",rx:"1"}),r("rect",{width:"7",height:"5",x:"3",y:"16",rx:"1"})]}),target:()=>R(P,{children:[r("circle",{cx:"12",cy:"12",r:"10"}),r("circle",{cx:"12",cy:"12",r:"6"}),r("circle",{cx:"12",cy:"12",r:"2"})]}),users:()=>R(P,{children:[r("path",{d:"M16 21v-2a4 4 0 0 0-4-4H6a4 4 0 0 0-4 4v2"}),r("path",{d:"M16 3.128a4 4 0 0 1 0 7.744"}),r("path",{d:"M22 21v-2a4 4 0 0 0-3-3.87"}),r("circle",{cx:"9",cy:"7",r:"4"})]}),wallet:()=>R(P,{children:[r("path",{d:"M19 7V4a1 1 0 0 0-1-1H5a2 2 0 0 0 0 4h15a1 1 0 0 1 1 1v4h-3a2 2 0 0 0 0 4h3a1 1 0 0 0 1-1v-2a1 1 0 0 0-1-1"}),r("path",{d:"M3 5v14a2 2 0 0 0 2 2h15a1 1 0 0 0 1-1v-4"})]}),route:()=>R(P,{children:[r("circle",{cx:"6",cy:"19",r:"3"}),r("path",{d:"M9 19h8.5a3.5 3.5 0 0 0 0-7h-11a3.5 3.5 0 0 1 0-7H15"}),r("circle",{cx:"18",cy:"5",r:"3"})]}),"radio-tower":()=>R(P,{children:[r("path",{d:"M4.9 16.1C1 12.2 1 5.8 4.9 1.9"}),r("path",{d:"M7.8 4.7a6.14 6.14 0 0 0-.8 7.5"}),r("circle",{cx:"12",cy:"9",r:"2"}),r("path",{d:"M16.2 4.8c2 2 2.26 5.11.8 7.47"}),r("path",{d:"M19.1 1.9a9.96 9.96 0 0 1 0 14.1"}),r("path",{d:"M9.5 18h5"}),r("path",{d:"m8 22 4-11 4 11"})]}),sparkles:()=>R(P,{children:[r("path",{d:"M11.017 2.814a1 1 0 0 1 1.966 0l1.051 5.558a2 2 0 0 0 1.594 1.594l5.558 1.051a1 1 0 0 1 0 1.966l-5.558 1.051a2 2 0 0 0-1.594 1.594l-1.051 5.558a1 1 0 0 1-1.966 0l-1.051-5.558a2 2 0 0 0-1.594-1.594l-5.558-1.051a1 1 0 0 1 0-1.966l5.558-1.051a2 2 0 0 0 1.594-1.594z"}),r("path",{d:"M20 2v4"}),r("path",{d:"M22 4h-4"}),r("circle",{cx:"4",cy:"20",r:"2"})]}),bot:()=>R(P,{children:[r("path",{d:"M12 8V4H8"}),r("rect",{width:"16",height:"12",x:"4",y:"8",rx:"2"}),r("path",{d:"M2 14h2"}),r("path",{d:"M20 14h2"}),r("path",{d:"M15 13v2"}),r("path",{d:"M9 13v2"})]}),"flask-conical":()=>R(P,{children:[r("path",{d:"M14 2v6a2 2 0 0 0 .245.96l5.51 10.08A2 2 0 0 1 18 22H6a2 2 0 0 1-1.755-2.96l5.51-10.08A2 2 0 0 0 10 8V2"}),r("path",{d:"M6.453 15h11.094"}),r("path",{d:"M8.5 2h7"})]}),"shield-check":()=>R(P,{children:[r("path",{d:"M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z"}),r("path",{d:"m9 12 2 2 4-4"})]}),"sliders-horizontal":()=>R(P,{children:[r("path",{d:"M10 5H3"}),r("path",{d:"M12 19H3"}),r("path",{d:"M14 3v4"}),r("path",{d:"M16 17v4"}),r("path",{d:"M21 12h-9"}),r("path",{d:"M21 19h-5"}),r("path",{d:"M21 5h-7"}),r("path",{d:"M8 10v4"}),r("path",{d:"M8 12H3"})]}),scale:()=>R(P,{children:[r("path",{d:"m16 16 3-8 3 8c-.87.65-1.92 1-3 1s-2.13-.35-3-1Z"}),r("path",{d:"m2 16 3-8 3 8c-.87.65-1.92 1-3 1s-2.13-.35-3-1Z"}),r("path",{d:"M7 21h10"}),r("path",{d:"M12 3v18"}),r("path",{d:"M3 7h2c2 0 5-1 7-2 2 1 5 2 7 2h2"})]}),plug:()=>R(P,{children:[r("path",{d:"M12 22v-5"}),r("path",{d:"M15 8V2"}),r("path",{d:"M17 8a1 1 0 0 1 1 1v4a4 4 0 0 1-4 4h-4a4 4 0 0 1-4-4V9a1 1 0 0 1 1-1z"}),r("path",{d:"M9 8V2"})]}),receipt:()=>R(P,{children:[r("path",{d:"M12 17V7"}),r("path",{d:"M16 8h-6a2 2 0 0 0 0 4h4a2 2 0 0 1 0 4H8"}),r("path",{d:"M4 3a1 1 0 0 1 1-1 1.3 1.3 0 0 1 .7.2l.933.6a1.3 1.3 0 0 0 1.4 0l.934-.6a1.3 1.3 0 0 1 1.4 0l.933.6a1.3 1.3 0 0 0 1.4 0l.933-.6a1.3 1.3 0 0 1 1.4 0l.934.6a1.3 1.3 0 0 0 1.4 0l.933-.6A1.3 1.3 0 0 1 19 2a1 1 0 0 1 1 1v18a1 1 0 0 1-1 1 1.3 1.3 0 0 1-.7-.2l-.933-.6a1.3 1.3 0 0 0-1.4 0l-.934.6a1.3 1.3 0 0 1-1.4 0l-.933-.6a1.3 1.3 0 0 0-1.4 0l-.933.6a1.3 1.3 0 0 1-1.4 0l-.934-.6a1.3 1.3 0 0 0-1.4 0l-.933.6a1.3 1.3 0 0 1-.7.2 1 1 0 0 1-1-1z"})]}),history:()=>R(P,{children:[r("path",{d:"M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"}),r("path",{d:"M3 3v5h5"}),r("path",{d:"M12 7v5l4 2"})]}),"trending-up":()=>R(P,{children:[r("path",{d:"M16 7h6v6"}),r("path",{d:"m22 7-8.5 8.5-5-5L2 17"})]}),"arrow-down-left":()=>R(P,{children:[r("path",{d:"M17 7 7 17"}),r("path",{d:"M17 17H7V7"})]}),"arrow-up-right":()=>R(P,{children:[r("path",{d:"M7 7h10v10"}),r("path",{d:"M7 17 17 7"})]}),"trending-down":()=>R(P,{children:[r("path",{d:"M16 17h6v-6"}),r("path",{d:"m22 17-8.5-8.5-5 5L2 7"})]}),pause:()=>R(P,{children:[r("rect",{x:"14",y:"3",width:"5",height:"18",rx:"1"}),r("rect",{x:"5",y:"3",width:"5",height:"18",rx:"1"})]}),play:()=>r("path",{d:"M5 5a2 2 0 0 1 3.008-1.728l11.997 6.998a2 2 0 0 1 .003 3.458l-12 7A2 2 0 0 1 5 19z"}),"undo-2":()=>R(P,{children:[r("path",{d:"M9 14 4 9l5-5"}),r("path",{d:"M4 9h10.5a5.5 5.5 0 0 1 5.5 5.5a5.5 5.5 0 0 1-5.5 5.5H11"})]}),"redo-2":()=>R(P,{children:[r("path",{d:"m15 14 5-5-5-5"}),r("path",{d:"M20 9H9.5A5.5 5.5 0 0 0 4 14.5A5.5 5.5 0 0 0 9.5 20H13"})]}),"trash-2":()=>R(P,{children:[r("path",{d:"M10 11v6"}),r("path",{d:"M14 11v6"}),r("path",{d:"M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"}),r("path",{d:"M3 6h18"}),r("path",{d:"M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"})]}),"grip-vertical":()=>R(P,{children:[r("circle",{cx:"9",cy:"12",r:"1"}),r("circle",{cx:"9",cy:"5",r:"1"}),r("circle",{cx:"9",cy:"19",r:"1"}),r("circle",{cx:"15",cy:"12",r:"1"}),r("circle",{cx:"15",cy:"5",r:"1"}),r("circle",{cx:"15",cy:"19",r:"1"})]}),"arrow-up":()=>R(P,{children:[r("path",{d:"m5 12 7-7 7 7"}),r("path",{d:"M12 19V5"})]}),"arrow-down":()=>R(P,{children:[r("path",{d:"M12 5v14"}),r("path",{d:"m19 12-7 7-7-7"})]}),zap:()=>r("path",{d:"M15.914 4a1.5 1.5 0 00-2.474-1.561l-9 9A1.5 1.5 0 005.5 14h4.002a.5.5 0 01.471.666L8.086 20a1.5 1.5 0 002.475 1.56l9-9A1.5 1.5 0 0018.5 10h-3.997a.5.5 0 01-.472-.667z"}),download:()=>R(P,{children:[r("path",{d:"M12 15V3"}),r("path",{d:"M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"}),r("path",{d:"m7 10 5 5 5-5"})]}),"refresh-cw":()=>R(P,{children:[r("path",{d:"M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8"}),r("path",{d:"M21 3v5h-5"}),r("path",{d:"M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16"}),r("path",{d:"M8 16H3v5"})]}),"credit-card":()=>R(P,{children:[r("rect",{width:"20",height:"14",x:"2",y:"5",rx:"2"}),r("line",{x1:"2",x2:"22",y1:"10",y2:"10"})]}),"file-text":()=>R(P,{children:[r("path",{d:"M6 22a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h8a2.4 2.4 0 0 1 1.704.706l3.588 3.588A2.4 2.4 0 0 1 20 8v12a2 2 0 0 1-2 2z"}),r("path",{d:"M14 2v5a1 1 0 0 0 1 1h5"}),r("path",{d:"M10 9H8"}),r("path",{d:"M16 13H8"}),r("path",{d:"M16 17H8"})]}),phone:()=>r("path",{d:"M13.832 16.568a1 1 0 0 0 1.213-.303l.355-.465A2 2 0 0 1 17 15h3a2 2 0 0 1 2 2v3a2 2 0 0 1-2 2A18 18 0 0 1 2 4a2 2 0 0 1 2-2h3a2 2 0 0 1 2 2v3a2 2 0 0 1-.8 1.6l-.468.351a1 1 0 0 0-.292 1.233 14 14 0 0 0 6.392 6.384"}),"phone-incoming":()=>R(P,{children:[r("path",{d:"M16 2v6h6"}),r("path",{d:"m22 2-6 6"}),r("path",{d:"M13.832 16.568a1 1 0 0 0 1.213-.303l.355-.465A2 2 0 0 1 17 15h3a2 2 0 0 1 2 2v3a2 2 0 0 1-2 2A18 18 0 0 1 2 4a2 2 0 0 1 2-2h3a2 2 0 0 1 2 2v3a2 2 0 0 1-.8 1.6l-.468.351a1 1 0 0 0-.292 1.233 14 14 0 0 0 6.392 6.384"})]}),"phone-outgoing":()=>R(P,{children:[r("path",{d:"m16 8 6-6"}),r("path",{d:"M22 8V2h-6"}),r("path",{d:"M13.832 16.568a1 1 0 0 0 1.213-.303l.355-.465A2 2 0 0 1 17 15h3a2 2 0 0 1 2 2v3a2 2 0 0 1-2 2A18 18 0 0 1 2 4a2 2 0 0 1 2-2h3a2 2 0 0 1 2 2v3a2 2 0 0 1-.8 1.6l-.468.351a1 1 0 0 0-.292 1.233 14 14 0 0 0 6.392 6.384"})]}),"phone-off":()=>R(P,{children:[r("path",{d:"M10.1 13.9a14 14 0 0 0 3.732 2.668 1 1 0 0 0 1.213-.303l.355-.465A2 2 0 0 1 17 15h3a2 2 0 0 1 2 2v3a2 2 0 0 1-2 2 18 18 0 0 1-12.728-5.272"}),r("path",{d:"M22 2 2 22"}),r("path",{d:"M4.76 13.582A18 18 0 0 1 2 4a2 2 0 0 1 2-2h3a2 2 0 0 1 2 2v3a2 2 0 0 1-.8 1.6l-.468.351a1 1 0 0 0-.292 1.233 14 14 0 0 0 .244.473"})]}),"mic-off":()=>R(P,{children:[r("path",{d:"M12 19v3"}),r("path",{d:"M15 9.34V5a3 3 0 0 0-5.68-1.33"}),r("path",{d:"M16.95 16.95A7 7 0 0 1 5 12v-2"}),r("path",{d:"M18.89 13.23A7 7 0 0 0 19 12v-2"}),r("path",{d:"m2 2 20 20"}),r("path",{d:"M9 9v3a3 3 0 0 0 5.12 2.12"})]}),smartphone:()=>R(P,{children:[r("rect",{width:"14",height:"20",x:"5",y:"2",rx:"2",ry:"2"}),r("path",{d:"M12 18h.01"})]}),"message-circle":()=>r("path",{d:"M2.992 16.342a2 2 0 0 1 .094 1.167l-1.065 3.29a1 1 0 0 0 1.236 1.168l3.413-.998a2 2 0 0 1 1.099.092 10 10 0 1 0-4.777-4.719"}),settings:()=>R(P,{children:[r("path",{d:"M12.22 2h-.44a2 2 0 0 0-2 1.72l-.12.87a2 2 0 0 1-1.18 1.52l-.26.15a2 2 0 0 1-1.9.18l-.81-.33a2 2 0 0 0-2.49.85l-.22.38a2 2 0 0 0 .49 2.57l.69.54a2 2 0 0 1 .72 1.77v.3a2 2 0 0 1-.72 1.77l-.69.54a2 2 0 0 0-.49 2.57l.22.38a2 2 0 0 0 2.49.85l.81-.33a2 2 0 0 1 1.9.18l.26.15a2 2 0 0 1 1.18 1.52l.12.87a2 2 0 0 0 2 1.72h.44a2 2 0 0 0 2-1.72l.12-.87a2 2 0 0 1 1.18-1.52l.26-.15a2 2 0 0 1 1.9-.18l.81.33a2 2 0 0 0 2.49-.85l.22-.39a2 2 0 0 0-.49-2.57l-.69-.53a2 2 0 0 1-.72-1.77v-.3a2 2 0 0 1 .72-1.77l.69-.54a2 2 0 0 0 .49-2.57l-.22-.38a2 2 0 0 0-2.49-.85l-.81.33a2 2 0 0 1-1.9-.18l-.26-.15a2 2 0 0 1-1.18-1.52l-.12-.87A2 2 0 0 0 12.22 2Z"}),r("circle",{cx:"12",cy:"12",r:"3"})]}),"circle-help":()=>R(P,{children:[r("circle",{cx:"12",cy:"12",r:"10"}),r("path",{d:"M9.09 9a3 3 0 0 1 5.83 1c0 2-3 3-3 3M12 17h.01"})]}),"shopping-bag":()=>R(P,{children:[r("path",{d:"m6 2-3 4v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2V6l-3-4Z"}),r("path",{d:"M3 6h18M16 10a4 4 0 0 1-8 0"})]}),hotel:()=>r(P,{children:r("path",{d:"M10 22V6.5a.5.5 0 0 0-.5-.5h-3a.5.5 0 0 1-.5-.5v-3a.5.5 0 0 1 .5-.5h11a.5.5 0 0 1 .5.5v3a.5.5 0 0 1-.5.5h-3a.5.5 0 0 0-.5.5V22M18 2v20M6 6v16M2 22h20M6 12H4a2 2 0 0 0-2 2v8M18 12h2a2 2 0 0 1 2 2v8"})}),globe:()=>R(P,{children:[r("circle",{cx:"12",cy:"12",r:"10"}),r("path",{d:"M2 12h20M12 2a17 17 0 0 1 0 20 17 17 0 0 1 0-20"})]}),"git-branch":()=>R(P,{children:[r("path",{d:"M6 3v12M18 9a6 6 0 0 1-6 6H6"}),r("circle",{cx:"6",cy:"18",r:"3"}),r("circle",{cx:"18",cy:"6",r:"3"})]}),calculator:()=>R(P,{children:[r("rect",{x:"4",y:"2",width:"16",height:"20",rx:"2"}),r("path",{d:"M8 6h8M8 10h2M14 10h2M8 14h2M14 14h2M8 18h2M14 18h2"})]}),plus:()=>r("path",{d:"M12 5v14M5 12h14"}),x:()=>r("path",{d:"m18 6-12 12M6 6l12 12"}),search:()=>R(P,{children:[r("circle",{cx:"11",cy:"11",r:"8"}),r("path",{d:"m21 21-4.3-4.3"})]}),check:()=>r("path",{d:"m20 6-11 11-5-5"}),minus:()=>r("path",{d:"M5 12h14"}),"chevron-down":()=>r("path",{d:"m6 9 6 6 6-6"}),"chevron-left":()=>r("path",{d:"m15 18-6-6 6-6"}),"chevron-right":()=>r("path",{d:"m9 18 6-6-6-6"}),"maximize-2":()=>r("path",{d:"M15 3h6v6M21 3l-7 7M3 21l7-7M9 21H3v-6"}),"minimize-2":()=>r("path",{d:"m14 10 7-7M20 10h-6V4M3 21l7-7M4 14h6v6"}),info:()=>R(P,{children:[r("circle",{cx:"12",cy:"12",r:"10"}),r("path",{d:"M12 16v-4M12 8h.01"})]}),"circle-check":()=>R(P,{children:[r("circle",{cx:"12",cy:"12",r:"10"}),r("path",{d:"m9 12 2 2 4-4"})]}),"triangle-alert":()=>R(P,{children:[r("path",{d:"m21.73 18-8-14a2 2 0 0 0-3.46 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3"}),r("path",{d:"M12 9v4M12 17h.01"})]}),"circle-alert":()=>R(P,{children:[r("circle",{cx:"12",cy:"12",r:"10"}),r("path",{d:"M12 8v4M12 16h.01"})]}),inbox:()=>R(P,{children:[r("path",{d:"M22 12h-6l-2 3h-4l-2-3H2"}),r("path",{d:"M5.45 5.11 2 12v6a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-6l-3.45-6.89A2 2 0 0 0 16.76 4H7.24a2 2 0 0 0-1.79 1.11Z"})]}),image:()=>R(P,{children:[r("rect",{width:"18",height:"18",x:"3",y:"3",rx:"2",ry:"2"}),r("circle",{cx:"9",cy:"9",r:"2"}),r("path",{d:"m21 15-3.086-3.086a2 2 0 0 0-2.828 0L6 21"})]}),lock:()=>R(P,{children:[r("rect",{width:"18",height:"11",x:"3",y:"11",rx:"2",ry:"2"}),r("path",{d:"M7 11V7a5 5 0 0 1 10 0v4"})]}),package:()=>R(P,{children:[r("path",{d:"M11 21.73a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73z"}),r("path",{d:"M12 22V12"}),r("path",{d:"m3.29 7 8.71 5 8.71-5"}),r("path",{d:"m7.5 4.27 9 5.15"})]})},at=e=>r("svg",{"data-ui":"icon","data-size":e.size??"default","data-tone":e.tone??"inherit",viewBox:"0 0 24 24",fill:"none",stroke:"currentColor","stroke-width":"2","stroke-linecap":"round","stroke-linejoin":"round",focusable:"false",role:e.label?"img":void 0,"aria-label":e.label??null,"aria-hidden":e.label?null:"true",children:Di[e.name]()});var ht=e=>{let t=e.as??"span";return r(t,{"data-ui":"text",id:e.id,"data-tone":e.tone??"default","data-variant":e.variant??null,"data-weight":e.fontWeight??null,"data-align":e.alignment??null,"data-numeric":e.numeric?"true":null,"data-truncate":e.truncate?"true":null,"data-break-word":e.breakWord?"true":null,"data-visually-hidden":e.visuallyHidden?"true":null,children:e.children})};var lr=e=>R(P,{children:[e.loading===!0&&r("span",{"data-ui":"action-spinner","aria-hidden":"true",children:r(da,{label:e.label,size:"small",decorative:!0})}),(e.leading!==void 0||e.icon!==void 0)&&r("span",{"data-ui":"action-leading","aria-hidden":"true",children:e.icon?r(at,{name:e.icon}):e.leading}),!e.iconOnly&&r("span",{"data-ui":"action-label","data-truncate":e.truncateLabel?"true":null,title:e.truncateLabel?e.label:void 0,children:r(ht,{truncate:e.truncateLabel,children:e.label})})]}),sr=e=>({id:e.id,"data-tone":e.tone??(e.variant==="destructive"?"danger":"default"),"data-loading":e.loading?"true":null,"data-full-width":e.fullWidth?"true":null,"data-viewport":e.viewport??null}),Gt=e=>r("button",{...sr(e),"data-ui":"action","data-variant":e.variant??"secondary","data-size":e.size??"default","data-logo":e.logo?"true":null,type:e.type??"button",name:e.name??null,value:e.value??null,form:e.form??null,formnovalidate:e.formNoValidate===!0?!0:void 0,disabled:e.disabled===!0||e.loading===!0,"aria-label":e.loading||e.logo?e.label:null,title:e.logo?e.label:void 0,"aria-busy":e.loading===!0?"true":null,"aria-pressed":e.pressed===void 0?null:String(e.pressed),"aria-describedby":e.describedBy??null,"aria-expanded":e.expanded===void 0?null:String(e.expanded),"aria-controls":e.controls??null,children:e.logo?R(P,{children:[e.loading===!0&&r("span",{"data-ui":"action-spinner","aria-hidden":"true",children:r(da,{label:e.label,size:"small",decorative:!0})}),r("img",{"data-ui":"action-logo",src:e.logo.src,alt:"",width:"80",height:"24"}),r("span",{"data-ui":"action-selection","aria-hidden":"true",children:r(at,{name:"check"})})]}):r(lr,{...e})}),It=e=>r("button",{...sr({...e,icon:void 0}),"data-ui":"action","data-icon-only":"true","data-variant":e.variant??"tertiary","data-size":e.size??"default",type:e.type??"button",name:e.name??null,value:e.value??null,form:e.form??null,formnovalidate:e.formNoValidate===!0?!0:void 0,disabled:e.disabled===!0||e.loading===!0,"aria-label":e.label,"aria-pressed":e.pressed===void 0?null:String(e.pressed),"aria-busy":e.loading===!0?"true":null,"aria-describedby":e.describedBy??null,"aria-expanded":e.expanded===void 0?null:String(e.expanded),"aria-controls":e.controls??null,title:e.label,children:r(lr,{...e,icon:void 0,leading:e.icon,iconOnly:!0})});var ca=e=>r("div",{"data-ui":"action-group",role:"group","aria-label":e.label??null,children:we(e.actions,(t,n)=>n,t=>r(P,{children:t}))});var dr=e=>e.map(t=>t.map(n=>/[\t\n\r"]/u.test(n)?`"${n.replace(/"/gu,'""')}"`:n).join("	")).join(`
`),cr=e=>{let t=e.replace(/\r\n?/gu,`
`),n=t.endsWith(`
`)?t.slice(0,-1):t,o=[],i=[],s="",m=!1,c=0;for(;c<n.length;){let y=n[c];m?y==='"'&&n[c+1]==='"'?(s+='"',c+=2):y==='"'?(m=!1,c+=1):(s+=y,c+=1):y==='"'&&s===""?(m=!0,c+=1):y==="	"?(i.push(s),s="",c+=1):y===`
`?(i.push(s),o.push(i),i=[],s="",c+=1):(s+=y,c+=1)}return i.push(s),o.push(i),o};var Ie=e=>e==null||e==="",gt=(e,t)=>Ie(e)&&Ie(t)?!0:typeof e=="number"&&typeof t=="number"?e===t||Number.isNaN(e)&&Number.isNaN(t):e!==null&&typeof e=="object"||t!==null&&typeof t=="object"?JSON.stringify(e)===JSON.stringify(t):e===t,Ke=e=>e.replace(/[đĐ]/gu,"d").normalize("NFD").replace(/[̀-ͯ]/gu,"").toLowerCase().trim(),ur=new Map,fa=(e,t)=>{let n=`${e}|${JSON.stringify(t)}`,o=ur.get(n);return o||(o=new Intl.NumberFormat(e,t),ur.set(n,o)),o},fr=new Map,Vi=e=>{let t=fr.get(e);if(!t){let n=new Intl.NumberFormat(e).formatToParts(12345.6);t={group:n.find(o=>o.type==="group")?.value??",",decimal:n.find(o=>o.type==="decimal")?.value??"."},fr.set(e,t)}return t},ji=(e,t)=>fa(t,{style:"currency",currency:e}).resolvedOptions().maximumFractionDigits??2,mr=e=>e.length>1&&e[0].length>=1&&e[0].length<=3&&e.slice(1).every(t=>t.length===3),hr=(e,t,n={})=>{let o=e.replace(/[\s  ]/gu,"").replace(/[₫$€£¥%]|VND|USD|đ$/giu,"");if(!o)return Ie(e.trim())?null:void 0;let i=/^([+\-−]?)((?:\d|[.,](?=\d))[\d.,]*)(k|tr)?$/iu.exec(o);if(!i)return;let[,s,m,c]=i;if(c&&!n.shorthand)return;let{group:y,decimal:b}=Vi(t),w=m.includes("."),E=m.includes(","),k=m,M="";if(w&&E){let f=m.lastIndexOf(".")>m.lastIndexOf(",")?".":",",V=f==="."?",":".",A=m.lastIndexOf(f),j=m.slice(0,A);if(j.includes(f)||!mr(j.split(V)))return;k=j.split(V).join(""),M=m.slice(A+1)}else if(w||E){let f=w?".":",",V=m.split(f),A=mr(V);if(V.length===2&&!(f===y&&A)&&!(f!==b&&A))k=V[0],M=V[1];else if(A)k=V.join("");else return}if(!/^\d*$/u.test(k)||!/^\d*$/u.test(M))return;let C=+`${s==="-"||s==="\u2212"?"-":""}${k||"0"}.${M||"0"}`;if(!Number.isFinite(C))return;let K=c?c.toLowerCase()==="k"?1e3:1e6:1;return Math.round(C*K*1e9)/1e9},ua=(e,t,n={})=>fa(t,{minimumFractionDigits:n.minimum??0,maximumFractionDigits:Math.max(n.minimum??0,n.maximum??2),useGrouping:n.grouping??!0}).format(e),vr=/^(\d{4})-(\d{2})-(\d{2})$/u,gr=(e,t,n)=>{let o=new Date(Date.UTC(e,t-1,n));if(!(o.getUTCFullYear()!==e||o.getUTCMonth()!==t-1||o.getUTCDate()!==n))return`${String(e).padStart(4,"0")}-${String(t).padStart(2,"0")}-${String(n).padStart(2,"0")}`},pr=new Map,Bi=e=>{let t=pr.get(e);return t||(t=new Intl.DateTimeFormat(e,{year:"numeric",month:"2-digit",day:"2-digit"}).formatToParts(new Date(Date.UTC(2026,10,23))).map(n=>n.type).filter(n=>n==="day"||n==="month"||n==="year"),t.length!==3&&(t=["day","month","year"]),pr.set(e,t)),t},qi=(e,t)=>{let n=e.trim();if(!n)return null;let o=vr.exec(n);if(o)return gr(Number(o[1]),Number(o[2]),Number(o[3]));let i=n.split(/[/.\-\s]+/u);if(i.length!==3||i.some(c=>!/^\d{1,4}$/u.test(c)))return;let s={};Bi(t).forEach((c,y)=>{s[c]=Number(i[y])});let m=s.year<100?2e3+s.year:s.year;return gr(m,s.month,s.day)},br=new Map,zi=(e,t)=>{let n=vr.exec(e);if(!n)return e;let o=br.get(t);return o||(o=new Intl.DateTimeFormat(t,{day:"2-digit",month:"2-digit",year:"numeric",timeZone:"UTC"}),br.set(t,o)),o.format(new Date(Date.UTC(Number(n[1]),Number(n[2])-1,Number(n[3]))))},_i=new Set(["true","1","yes","y","x","\u2713","co","dung"]),Ji=new Set(["false","0","no","n","","khong","sai"]),Xi=(e,t)=>{let n=Ke(e);if(_i.has(n)||t&&n===Ke(t.yes))return!0;if(Ji.has(n)||t&&n===Ke(t.no))return!1},ma=(e,t)=>{let n=e.trim(),o=t.find(s=>s.value===n);if(o)return o;let i=Ke(n);return t.find(s=>Ke(s.label)===i)},Ui=(e,t)=>{if(Ie(e))return"";let n=String(e);return t.options?.find(o=>o.value===n)?.label??t.learned?.(n)??n},wr=e=>e.fractionDigits??ji(e.currency,e.locale),on=(e,t)=>{if(t.type==="checkbox")return e===!0?t.yes:t.no;if(Ie(e))return"";switch(t.type){case"number":{if(typeof e!="number")return String(e);let n=t.fractionDigits;return ua(e,t.locale,{minimum:n??0,maximum:n??2})}case"money":{if(typeof e!="number")return String(e);let n=wr(t);return ua(e,t.locale,{minimum:n,maximum:n})}case"percent":return typeof e=="number"?fa(t.locale,{style:"percent",minimumFractionDigits:t.fractionDigits??0,maximumFractionDigits:t.fractionDigits??2}).format(e/100):String(e);case"date":return zi(String(e),t.locale);case"select":case"combobox":return Ui(e,t);default:return String(e)}},kr=(e,t)=>Ie(e)?"":(t.type==="number"||t.type==="money"||t.type==="percent")&&typeof e=="number"?ua(e,t.locale,{maximum:10,grouping:!1}):t.type==="date"?String(e):on(e,t),xr=(e,t)=>{switch(t.type){case"number":case"percent":{let n=hr(e,t.locale);return n===void 0?{ok:!1,reason:"number"}:{ok:!0,value:n}}case"money":{let n=hr(e,t.locale,{shorthand:!0});if(n===void 0)return{ok:!1,reason:"number"};if(n===null)return{ok:!0,value:n};let o=10**wr(t);return{ok:!0,value:Math.round(n*o)/o}}case"date":{let n=qi(e,t.locale);return n===void 0?{ok:!1,reason:"date"}:{ok:!0,value:n}}case"checkbox":{let n=Xi(e,t);return n===void 0?{ok:!1,reason:"boolean"}:{ok:!0,value:n}}case"select":case"combobox":{if(!e.trim())return{ok:!0,value:null};let n=ma(e,t.options??[]);if(n)return{ok:!0,value:n.value};let o=t.learned?.(e.trim());return o!=null?{ok:!0,value:e.trim()}:{ok:!1,reason:"option"}}default:return{ok:!0,value:e.replace(/\r?\n$/u,"")}}},yr=new Map,Wi=e=>{let t=yr.get(e);return t||(t=new Intl.Collator(e,{sensitivity:"base",numeric:!0}),yr.set(e,t)),t},Rr=(e,t,n)=>{let o=Ie(e),i=Ie(t);return o||i?o===i?0:o?1:-1:typeof e=="number"&&typeof t=="number"?e-t:typeof e=="boolean"&&typeof t=="boolean"?Number(e)-Number(t):n.type==="date"?String(e)<String(t)?-1:String(e)>String(t)?1:0:Wi(n.locale).compare(on(e,n),on(t,n))};var Zi={text:200,number:112,money:136,percent:96,date:136,select:176,combobox:224,checkbox:72},Yi=new Set(["number","money","percent"]),Mr=e=>e==null?null:typeof e=="string"||typeof e=="number"||typeof e=="boolean"?e:String(e),Er=(e,t)=>{let n=e.type??"text",o=e.field??e.key,i=new Map,s={type:n,locale:t.locale,currency:e.currency??t.currency,fractionDigits:e.fractionDigits,options:e.options??[],learned:w=>i.get(w)??null,yes:t.yes,no:t.no},m=typeof e.value=="function",c=m?w=>Mr(e.value?.(w)):w=>Mr(w[o]),y=m&&!e.set?()=>!1:typeof e.editable=="function"?e.editable:()=>e.editable===!0,b=w=>{let E=0,k=!1;for(let M of w){let C=c(M);typeof C=="number"&&Number.isFinite(C)&&(E+=C,k=!0)}return k?Math.round(E*1e9)/1e9:null};return{key:e.key,field:o,label:e.label,type:n,width:Math.max(e.minWidth??48,e.width??Zi[n]),grow:e.pinned?0:Math.max(0,e.grow??0),minWidth:e.minWidth??48,pinned:e.pinned??null,align:e.align??(Yi.has(n)?"end":n==="checkbox"?"center":"start"),computed:m,computedWriter:m&&!!e.set&&!!e.editable,editable:y,required:e.required===!0,min:typeof e.min=="number"?e.min:null,max:typeof e.max=="number"?e.max:null,options:e.options??[],choices:e.optionsFor??(()=>e.options??[]),search:e.search??null,placeholder:e.placeholder??null,hint:e.hint??null,sortable:e.sortable===!0,resizable:e.resizable!==!1,spec:s,read:c,write:e.set??((w,E)=>({...w,[o]:E})),format:e.format??(w=>on(w,s)),parse:e.parse?(w,E)=>{let k=e.parse?.(w,E);return k===void 0?{ok:!1,reason:"value"}:{ok:!0,value:k}}:(w,E)=>{let k=xr(w,s);return k.ok&&e.optionsFor&&!Ie(k.value)&&!e.optionsFor(E).some(M=>M.value===String(k.value))?{ok:!1,reason:"option"}:k},validate:e.validate??null,footer:e.footer==="sum"?b:e.footer??null,learned:i}},Sr=e=>e.type==="checkbox"?!1:e.type==="text"?"":null,Tr=(e,t)=>e.type==="checkbox"?!1:Ie(t);var Qi={ArrowUp:[-1,0],ArrowDown:[1,0],ArrowLeft:[0,-1],ArrowRight:[0,1]},Or=e=>{if(e.isComposing)return null;let t=e.ctrlKey===!0||e.metaKey===!0,n=e.shiftKey===!0,o=e.altKey===!0,i=Qi[e.key];if(i)return o&&!t&&i[1]===0?{type:"moveRows",direction:i[0]}:o?null:t?{type:"edge",rows:i[0],cols:i[1],extend:n}:{type:"move",rows:i[0],cols:i[1],extend:n};switch(e.key){case"Home":return t?{type:"edge",rows:-1,cols:-1,extend:n}:{type:"edge",rows:0,cols:-1,extend:n};case"End":return t?{type:"edge",rows:1,cols:1,extend:n}:{type:"edge",rows:0,cols:1,extend:n};case"PageUp":return{type:"page",direction:-1,extend:n};case"PageDown":return{type:"page",direction:1,extend:n};case"Tab":return t||o?null:{type:"tab",backwards:n};case"Enter":return t?{type:"insertRow"}:n||o?null:{type:"edit"};case"F2":return{type:"edit"};case"Escape":return{type:"collapse"};case"Delete":case"Backspace":return t?{type:"removeRows"}:o?null:{type:"clear"};case" ":return t||o?null:{type:"toggle"};case"Process":return{type:"type"}}if(t&&!o){let s=e.key.toLowerCase();return s==="a"?{type:"selectAll"}:s==="d"?{type:"fillDown"}:s==="z"?n?{type:"redo"}:{type:"undo"}:s==="y"?{type:"redo"}:null}return e.key.length===1?{type:"type"}:null},Cr=(e,t,n)=>Math.min(n,Math.max(t,e)),ln=(e,t,n,o)=>({row:Cr(e.row+t,o.header?-1:0,Math.max(o.header?-1:0,o.rows-1)),col:Cr(e.col+n,0,Math.max(0,o.cols-1))}),ha=(e,t,n,o)=>({row:t===0?e.row:t<0?0:Math.max(0,o.rows-1),col:n===0?e.col:n<0?0:Math.max(0,o.cols-1)}),ga=(e,t)=>({top:Math.min(e.row,t.row),left:Math.min(e.col,t.col),bottom:Math.max(e.row,t.row),right:Math.max(e.col,t.col)}),Lr=(e,t,n)=>t>=e.top&&t<=e.bottom&&n>=e.left&&n<=e.right,Ar=(e,t,n,o)=>{let i=n.rows*n.cols,s=Math.max(0,e.row)*n.cols+e.col;for(let m=0;m<i;m+=1){if(s+=t?-1:1,s<0||s>=i)return null;let c=Math.floor(s/n.cols),y=s%n.cols;if(o(c,y))return{row:c,col:y}}return null};var pa=(e,t)=>e.replace(/\{(\w+)\}/gu,(n,o)=>t[o]??n),ba=e=>e==null?null:typeof e=="string"||typeof e=="number"||typeof e=="boolean"?e:String(e),ya=(e,t,n)=>Math.min(n,Math.max(t,e)),Gr=e=>{let t=e.columns,n=new Map(t.map(h=>[h.key,h])),o=e.historyLimit??200,i=[],s=new Map,m=new Map,c=new Set,y=[],b=new Map,w=!1,E=null,k=[],M=[],C=null,K=0,f=0,V=0,A=new Map,j=(h,g)=>{let v=A.get(h);if(v&&v.version===f)return v.value;let T=g();return A.set(h,{version:f,value:T}),T},G=()=>{f+=1},Y=(h,g)=>({id:g,base:h,initial:h,row:h,touched:new Set,conflicts:new Map,removedElsewhere:!1}),D=h=>{i=[],s.clear(),m=new Map,c=new Set,y=[],b=new Map,w=!1,k=[],M=[];for(let g of h){let v=String(e.rowId(g));if(s.has(v))throw new Error(`KetGrid: two rows have the id "${v}".`);let T=Y(g,v);i.push(T),s.set(v,T),y.push(v)}G()};D(e.rows);let ne=h=>{K===0&&(C={reason:h,steps:[],cells:[]}),K+=1},Q=()=>{if(K-=1,K>0)return null;let h=C;return C=null,G(),h?.steps.length?(k.push(h),k.length>o&&k.shift(),M=[],{reason:h.reason,cells:h.cells}):null},ke=(h,g)=>{C?.steps.push(h),g&&C?.cells.push(g)},xe=(h,g)=>[...new Set([...Object.keys(h),...Object.keys(g)])].filter(v=>!gt(h[v],g[v])),me=(h,g)=>Object.fromEntries(g.map(v=>[v,h[v]])),F=(h,g)=>{h.row={...h.row,...g}},q=h=>h.base===null&&!h.removedElsewhere&&xe(h.initial,h.row).length===0,ae=(h,g)=>{let v=e.fullWidthKey?.(h.row)??null;return v===null||v===g.key},te=(h,g)=>!e.readOnly&&ae(h,g)&&e.rowEditable(h.row)&&g.editable(h.row),oe=h=>!e.readOnly&&e.rowRemovable(h.row),Pe=(h,g)=>!g.computed&&!gt(g.read(h.row),g.read(h.base??h.initial)),Se=()=>j("displaced",()=>{let h=y.filter(T=>s.get(T)?.base),g=new Map(h.map((T,H)=>[T,H])),v=i.filter(T=>T.base).map(T=>T.id);return new Set(v.filter((T,H)=>c.has(T)&&g.get(T)!==H))}),Re=h=>h.base===null||Se().has(h.id),_e=(h,g)=>{let v=b.get(h.id)?.get(g.key);if(v)return{tone:"danger",message:v};if(!g.computed&&h.conflicts.has(g.field)){let $=ba(h.conflicts.get(g.field)),B=g.format($,{...h.row,[g.field]:$});return{tone:"warning",message:pa(e.messages.conflict,{value:B}),theirs:$}}if(!te(h,g)||q(h)||!w&&!h.touched.has(g.key)||h.base&&!Pe(h,g)&&!h.touched.has(g.key))return null;let T=g.read(h.row);if(g.required&&Tr(g,T))return{tone:"danger",message:e.messages.required};if(typeof T=="number"){if(g.min!==null&&T<g.min)return{tone:"danger",message:pa(e.messages.belowMin,{min:g.format(g.min,h.row)})};if(g.max!==null&&T>g.max)return{tone:"danger",message:pa(e.messages.aboveMax,{max:g.format(g.max,h.row)})}}let H=g.validate?.(T,h.row);return H?{tone:"danger",message:H}:null},$e=h=>b.has(h.id)||h.conflicts.size>0||h.removedElsewhere||(h.base===null?!q(h):h.touched.size>0||xe(h.base,h.row).length>0),Ae=()=>j("issues",()=>{let h=[];for(let g of i)if($e(g)){g.removedElsewhere&&h.push({id:g.id,key:t[0]?.key??"",tone:"warning",message:e.messages.removedElsewhere});for(let v of t){let T=_e(g,v);T&&h.push({id:g.id,key:v.key,tone:T.tone,message:T.message})}}return h}),Ne=()=>j("changes",()=>{let h=[],g=[],v=[];for(let N of i){if(N.base===null){if(q(N))continue;h.push({id:N.id,index:v.length,row:N.row}),v.push(N);continue}v.push(N);let _=xe(N.base,N.row);_.length&&g.push({id:N.id,row:N.row,fields:_})}let T=y.filter(N=>s.get(N)?.base),H=v.filter(N=>N.base).map(N=>N.id),$=v.findIndex(N=>N.base===null),B=$<0||v.slice($).every(N=>N.base===null),J=T.length!==H.length||T.some((N,_)=>N!==H[_]);return{added:h,updated:g,removed:[...m.keys()],order:!J&&B?null:v.map(N=>N.id)}}),it=()=>j("summary",()=>{let h=0;for(let v of i)if(v.base)for(let T of t)Pe(v,T)&&(h+=1);let g=Ae();return{added:Ne().added.length,edited:h,removed:m.size,errors:g.filter(v=>v.tone==="danger").length,conflicts:g.filter(v=>v.tone==="warning").length}}),Ye=()=>j("view",()=>{let h=E?n.get(E.key):null;if(!E||!h)return i.slice();let g=E.direction==="asc"?1:-1;return i.map((v,T)=>({entry:v,at:T,value:h.read(v.row)})).sort((v,T)=>{let H=Ie(v.value),$=Ie(T.value);return H!==$?H?1:-1:Rr(v.value,T.value,h.spec)*g||v.at-T.at}).map(v=>v.entry)}),bt=(h,g)=>{ne(g);let v=0;for(let T of h){let H=s.get(T.id),$=n.get(T.key);if(!H||!$||!te(H,$)){v+=1;continue}H.touched.add($.key);let B=H.row,J=$.write(B,T.value),N=xe(B,J);if(!N.length)continue;let _=me(B,N),yt=me(J,N);H.row=J,ke({undo:()=>F(H,_),redo:()=>F(H,yt)},{id:H.id,key:$.key});for(let ie of N)H.conflicts.delete(ie);let dt=b.get(H.id);if(dt)for(let ie of t)(ie.key===$.key||N.includes(ie.field))&&dt.delete(ie.key)}return{change:Q(),skipped:v}},Qe=(h,g)=>{let v=s.get(h);if(!v)return null;ne("patch");let T=xe(v.row,{...v.row,...g});if(T.length){let H=me(v.row,T),$=me(g,T);F(v,$),ke({undo:()=>F(v,H),redo:()=>F(v,$)})}return Q()},ot=(h,g)=>{for(let v of t){if(!(v.field in g)||!te(h,v))continue;let T=h.row;h.row=v.write(T,ba(g[v.field])),xe(T,h.row).length&&h.touched.add(v.key)}if(!e.readOnly&&e.rowEditable(h.row)&&!e.fullWidthKey?.(h.row))for(let v of e.draftFlags??[])typeof g[v]=="boolean"&&(h.row={...h.row,[v]:g[v]})},Je=h=>{let g=[];for(let v of h){let T=s.get(v.id);if(!T){m.has(v.id)||g.push(v);continue}T.base!==null&&ot(T,v.values)}return G(),g},Fe=(h,g,v)=>{if(e.readOnly)return null;let T=g??e.createRow?.({index:h});if(!T)return null;let H=v&&!s.has(v)&&!m.has(v)?v:"";for(;!H||s.has(H)||m.has(H);)V+=1,H=`new-${V}`;let $={id:H,base:null,initial:T,row:T,touched:new Set,conflicts:new Map,removedElsewhere:!1},B=ya(h,0,i.length),J=()=>{i.splice(Math.min(B,i.length),0,$),s.set($.id,$)},N=()=>{let _=i.indexOf($);_>=0&&i.splice(_,1),s.delete($.id)};return ne("add"),J(),ke({undo:N,redo:J},{id:$.id,key:t[0]?.key??""}),Q(),$.id},xt=h=>{let g=[...new Set(h)].map($=>s.get($)).filter($=>!!$&&oe($)).sort(($,B)=>i.indexOf($)-i.indexOf(B));if(!g.length)return[];let v=g.map($=>i.indexOf($)),T=()=>{for(let $=g.length-1;$>=0;$-=1){let B=g[$],J=i.indexOf(B);J>=0&&i.splice(J,1),s.delete(B.id),B.base&&m.set(B.id,{entry:B,index:v[$]})}},H=()=>{g.forEach(($,B)=>{i.splice(Math.min(v[B],i.length),0,$),s.set($.id,$),m.delete($.id)})};return ne("remove"),T(),ke({undo:H,redo:T}),Q(),g.map($=>$.id)},je=(h,g)=>{let v=i.filter(H=>!h.includes(H)),T=ya(g,0,v.length);i=[...v.slice(0,T),...h,...v.slice(T)]},lt=(h,g)=>{if(e.readOnly)return!1;let v=i.filter(N=>h.includes(N.id));if(!v.length)return!1;let T=i.indexOf(v[0]),H=v.every((N,_)=>i[T+_]===N),$=ya("by"in g?T+g.by:g.to,0,i.length-v.length);if($===T&&H)return!1;let B=v.filter(N=>!c.has(N.id)).map(N=>N.id),J=N=>{for(let _ of B)N?c.add(_):c.delete(_)};return ne("move"),je(v,$),J(!0),ke({undo:()=>{je(v,T),J(!1)},redo:()=>{je(v,$),J(!0)}},{id:v[0].id,key:t[0]?.key??""}),Q(),!0},st=h=>{let g=h==="undo"?k:M,v=h==="undo"?M:k,T=g.pop();if(!T)return null;if(h==="undo")for(let H=T.steps.length-1;H>=0;H-=1)T.steps[H].undo();else for(let H of T.steps)H.redo();return v.push(T),G(),{reason:T.reason,cells:T.cells}};return{columns:t,column:h=>n.get(h),entry:h=>s.get(h),entries:()=>i,view:Ye,version:()=>f,canEdit:te,canRemove:oe,canInsert:()=>!e.readOnly&&e.createRow!==null,cellDirty:Pe,rowUnsaved:Re,cellIssue:_e,pristine:q,issues:Ae,changes:Ne,summary:it,edit:bt,patchRow:Qe,restore:Je,restoreDraft:(h,g)=>{if(e.readOnly)return[];if(g.remove&&xt(h.removed),g.add)for(let T of h.added){if(s.has(T.id)||m.has(T.id))continue;let H=e.createRow?.({index:T.index})??{},$=Fe(T.index,H,T.id),B=$?s.get($):null;B&&ot(B,T.values)}let v=Je(h.updated);return g.order&&h.order&&h.order.forEach((T,H)=>{s.has(T)&&lt([T],{to:H})}),k=[],M=[],v},setReadOnly:h=>{e.readOnly=h,G()},insert:Fe,remove:xt,move:lt,undo:()=>e.readOnly?null:st("undo"),redo:()=>e.readOnly?null:st("redo"),canUndo:()=>!e.readOnly&&k.length>0,canRedo:()=>!e.readOnly&&M.length>0,batch:(h,g)=>{ne(h);try{return{value:g(),change:Q()}}catch(v){throw Q(),v}},sort:()=>E,setSort:h=>{E=h,G()},apply:h=>{let g=[],v=[],T=[],H=0,$=B=>{let J=String(e.rowId(B)),N=s.get(J)??m.get(J)?.entry;if(!N){let ie=Y(B,J);i.push(ie),s.set(J,ie),y.push(J),v.push(J);for(let u of t)g.push({id:J,key:u.key});return}let _=N.base??N.initial,yt=N.row,dt={...N.row};for(let ie of new Set([...Object.keys(_),...Object.keys(B)])){let u=B[ie];gt(_[ie],u)||(gt(N.row[ie],_[ie])?dt[ie]=u:gt(N.row[ie],u)?N.conflicts.delete(ie):(N.conflicts.set(ie,u),H+=1))}if(N.base===null&&!y.includes(J)&&y.push(J),N.base=B,N.removedElsewhere=!1,N.row=dt,s.has(J))for(let ie of t)gt(ie.read(yt),ie.read(N.row))||g.push({id:J,key:ie.key})};for(let B of h.update??[])$(B);for(let B of h.add??[])$(B);for(let B of h.remove??[]){if(y=y.filter(_=>_!==B),m.delete(B))continue;let J=s.get(B);if(!J)continue;J.base!==null&&!xe(J.base,J.row).length&&!J.conflicts.size?(i=i.filter(_=>_!==J),s.delete(B),b.delete(B),T.push(B)):(J.base=null,J.removedElsewhere=!0)}return G(),{changed:g,added:v,removed:T,conflicts:H}},resolve:(h,g,v)=>{let T=s.get(h),H=n.get(g);if(!T||!H||!T.conflicts.has(H.field))return null;let $=ba(T.conflicts.get(H.field));return T.conflicts.delete(H.field),v==="mine"?(G(),{reason:"resolve",cells:[{id:h,key:g}]}):Qe(h,{[H.field]:$})??{reason:"resolve",cells:[{id:h,key:g}]}},setErrors:h=>{b=new Map(Object.entries(h).map(([g,v])=>[g,new Map(Object.entries(v))])),G()},revealIssues:()=>(w=!0,G(),Ae()),totals:()=>j("totals",()=>{let h=i.map(v=>v.row),g=new Map;for(let v of t)v.footer&&g.set(v.key,v.footer(h));return g}),reset:h=>{let g=E;D(h),E=g},markSaved:h=>{if(h){let g=E;D(h),E=g;return}i=i.filter(g=>!q(g));for(let g of i)g.base=g.row,g.initial=g.row,g.touched.clear(),g.conflicts.clear(),g.removedElsewhere=!1;s.clear();for(let g of i)s.set(g.id,g);m=new Map,c=new Set,y=i.map(g=>g.id),b=new Map,w=!1,k=[],M=[],G()},revert:()=>{let h=y.map(v=>s.get(v)??m.get(v)?.entry).filter(v=>!!v?.base).map(v=>v.base),g=E;D(h),E=g}}};var va=(e,t,n=6)=>{if(t<=0||e.rowHeight<=0)return{start:0,end:0};let o=Math.max(0,e.scrollTop),i=Math.max(e.rowHeight,e.height-e.header-e.footer),s=Math.max(0,Math.floor(o/e.rowHeight)-n),m=Math.min(t,Math.ceil((o+i)/e.rowHeight)+n);return{start:s,end:Math.max(s,m)}},Ir=(e,t)=>{let n=t*e.rowHeight,o=n+e.rowHeight,i=Math.max(e.rowHeight,e.height-e.header-e.footer);return n<e.scrollTop?n:o>e.scrollTop+i?o-i:null},Kr=e=>Math.max(1,Math.floor((e.height-e.header-e.footer)/Math.max(1,e.rowHeight))-1);var Pr=JSON.stringify({added:[],updated:[],removed:[],order:null}),dn={loading:"Loading rows\u2026",empty:"No rows yet",addRow:"Add row",removeRows:"Remove rows",undo:"Undo",redo:"Redo",total:"Total",dragHandle:"Drag to reorder",instructions:"Arrow keys move between cells. Enter or F2 edits, Escape cancels, Tab moves to the next editable cell. Ctrl+Z undoes, Ctrl+D fills down, Alt+arrow moves a row.",required:"Required",invalidNumber:"Enter a number",invalidDate:"Enter a valid date",invalidOption:"Choose a value from the list",invalidValue:"Enter a valid value",belowMin:"At least {min}",aboveMax:"At most {max}",conflict:"Changed elsewhere to \u201C{value}\u201D.",keepMine:"Keep mine",takeTheirs:"Use new value",removedElsewhere:"This row was removed elsewhere. Saving adds it back.",searching:"Searching\u2026",noOptions:"No matches",searchFailed:"Could not load choices",addedRows:"{count} new",editedCells:"{count} edited",removedRows:"{count} removed",otherPages:"{count} changed on other pages",errors:"{count} to fix",conflicts:"{count} changed elsewhere",copied:"Cells copied: {count}",pasted:"Cells pasted: {count}",pasteSkipped:"Cells not pasted: {count}",rowAdded:"Row added",rowsRemoved:"Rows removed: {count}",fixBeforeSaving:"Cells to fix before saving: {count}",rowMoved:"Row moved",undone:"Undone",redone:"Redone",sortedAscending:"Sorted by {column}, ascending",sortedDescending:"Sorted by {column}, descending",sortCleared:"Sort cleared",readOnlyCell:"This cell cannot be edited",yes:"Yes",no:"No"},re="\0header",eo=0,Ze=(e,t)=>e.replace(/\{(\w+)\}/gu,(n,o)=>t[o]===void 0?n:String(t[o])),sn=e=>Bn(e),Cn=(e,t="small")=>sn(at({name:e,size:t})),X=(e,t={},n)=>{let o=document.createElement(e);for(let[i,s]of Object.entries(t))s!=null&&o.setAttribute(i,String(s));return n!==void 0&&(o.textContent=n),o},ue=(e,t,n)=>{n===null?e.hasAttribute(t)&&e.removeAttribute(t):e.getAttribute(t)!==n&&e.setAttribute(t,n)},to=()=>typeof document<"u"&&document.documentElement.lang||"en";function wa(e,t){let n={...dn,...t.labels},o=t.locale??to(),i=t.currency??"VND",s=`ket-grid-${eo+=1}`,m=new AbortController,{signal:c}=m,y=t.reorderable===!0&&t.readOnly!==!0,b=t.removable,w=typeof b=="function"?b:()=>b===!0&&t.readOnly!==!0,E=t.newRows??(t.createRow?[{label:n.addRow,create:t.createRow}]:[]),k=t.columns.map(a=>Er(a,{locale:o,currency:i,yes:n.yes,no:n.no}));for(let a of k)for(let[l,d]of Object.entries(t.carry?.optionLabels?.[a.key]??{}))typeof d=="string"&&a.learned.set(l,d);let M=[...k.filter(a=>a.pinned==="start"),...k.filter(a=>a.pinned===null),...k.filter(a=>a.pinned==="end")],C=M.some(a=>a.footer!==null),K=t.readOnly!==!0&&(M.some(a=>!a.computed||a.computedWriter)||E.length>0||b!==void 0),f=Gr({columns:M,rows:t.rows,draftFlags:t.draftFlags,rowId:t.rowId,createRow:E[0]?.create??null,rowEditable:t.rowEditable??(()=>!0),rowRemovable:w,fullWidthKey:t.fullWidth?a=>t.fullWidth?.(a)?.key??null:void 0,readOnly:t.readOnly===!0,messages:{required:n.required,belowMin:n.belowMin,aboveMax:n.aboveMax,conflict:n.conflict,removedElsewhere:n.removedElsewhere}}),V=t.carry?f.restoreDraft(t.carry,{add:E.length>0,remove:!!t.removable,order:t.reorderable===!0}):[],A=X("div",{"data-ui":"ket-grid","data-density":t.density??"default","data-editable":K?"true":"false"}),j=X("div",{"data-ui":"visually-hidden",id:`${s}-help`},n.instructions),G=X("div",{"data-ui":"kg-viewport",role:"grid","aria-label":t.label,"aria-describedby":j.id,"aria-multiselectable":"true","aria-readonly":K?null:"true",tabindex:"0"});t.height&&(G.style.height=t.height,A.dataset.fixedHeight="true");let Y=X("div",{"data-ui":"kg-header",role:"rowgroup"}),D=X("div",{"data-ui":"kg-row",role:"row","aria-rowindex":"1","data-kind":"header"});Y.append(D);let ne=X("div",{"data-ui":"kg-body",role:"rowgroup"}),Q=X("div",{"data-ui":"kg-spacer","aria-hidden":"true"}),ke=X("div",{"data-ui":"kg-spacer","aria-hidden":"true"}),xe=X("div",{"data-ui":"kg-empty",role:"row"}),me=X("div",{role:"gridcell","aria-colspan":String(M.length)},n.empty);xe.append(me);let F=X("div",{"data-ui":"kg-drop","aria-hidden":"true",hidden:""});ne.append(Q,ke,F);let q=X("div",{"data-ui":"kg-footer",role:"rowgroup"}),ae=X("div",{"data-ui":"kg-row",role:"row","data-kind":"footer"});q.append(ae),G.append(Y,ne),C&&G.append(q);let te=X("div",{"data-ui":"kg-message",id:`${s}-message`,hidden:""}),oe=X("div",{"data-ui":"kg-tools"}),Pe=X("div",{"data-ui":"kg-status"}),Se=X("div",{"data-ui":"visually-hidden",role:"status","aria-live":"polite"}),Re=t.name?X("input",{"data-ui":"kg-value",type:"text",hidden:"",tabindex:"-1","aria-hidden":"true",autocomplete:"off",name:t.name,form:t.form??null}):null;A.append(j,G,te,oe,Se),Re&&A.append(Re),K||(oe.hidden=!0);let _e=(a,l,d,p=!1)=>sn(p?It({label:l,icon:at({name:d}),variant:"tertiary",name:"kg-command",value:a}):Gt({label:l,icon:d,variant:"tertiary",name:"kg-command",value:a})),$e=typeof navigator<"u"&&/Mac|iPhone|iPad|iPod/u.test(navigator.platform||navigator.userAgent);oe.innerHTML=[...E.map((a,l)=>_e(`add-${l}`,a.label,"plus")),b!==void 0?_e("remove",n.removeRows,"trash-2"):"",_e("undo",`${n.undo} (${$e?"\u2318Z":"Ctrl+Z"})`,"undo-2",!0),_e("redo",`${n.redo} (${$e?"\u21E7\u2318Z":"Ctrl+Shift+Z"})`,"redo-2",!0)].join(""),oe.append(Pe);let Ae=a=>oe.querySelector(`button[name="kg-command"][value="${a}"]`),Ne=28,it=new Map,Ye=a=>it.get(a.key)??a.width,bt=()=>[...y?[`${Ne}px`]:[],...M.map(a=>a.grow>0&&!it.has(a.key)?`minmax(${Ye(a)}px, ${a.grow}fr)`:`${Ye(a)}px`)].join(" "),Qe=()=>(y?Ne:0)+M.reduce((a,l)=>a+Ye(l),0),ot=()=>{let a=new Map,l=y?Ne:0;for(let p of M)p.pinned==="start"&&(a.set(p.key,{side:"left",offset:l}),l+=Ye(p));let d=0;for(let p of[...M].reverse())p.pinned==="end"&&(a.set(p.key,{side:"right",offset:d}),d+=Ye(p));return a},Je=ot(),Fe=()=>(y?Ne:0)+M.filter(a=>a.pinned==="start").reduce((a,l)=>a+Ye(l),0),xt=()=>M.filter(a=>a.pinned==="end").reduce((a,l)=>a+Ye(l),0),je=()=>{Je=ot(),A.style.setProperty("--kg-template",bt()),A.style.setProperty("--kg-min-width",`${Qe()}px`);for(let a of A.querySelectorAll("[data-kg-key]"))lt(a,a.dataset.kgKey??"")},lt=(a,l)=>{let d=Je.get(l);a.style.left=d?.side==="left"?`${d.offset}px`:"",a.style.right=d?.side==="right"?`${d.offset}px`:"",ue(a,"data-pinned",d?d.side==="left"?"start":"end":null)},st=[];y&&D.append(X("div",{"data-ui":"kg-handle","aria-hidden":"true"})),M.forEach((a,l)=>{let d=X("div",{"data-ui":"kg-header-cell",role:"columnheader","aria-colindex":String(l+1),"data-kg-key":a.key,"data-align":a.align,"data-sortable":a.sortable?"true":null,tabindex:"-1",title:a.hint});if(d.append(X("span",{"data-ui":"kg-header-label"},a.label)),a.sortable){let p=X("span",{"data-ui":"kg-sort","aria-hidden":"true"});d.append(p)}a.resizable&&d.append(X("span",{"data-ui":"kg-resize","aria-hidden":"true"})),st.push(d),D.append(d)});let $t=[];y&&ae.append(X("div",{"data-ui":"kg-handle","aria-hidden":"true"})),M.forEach((a,l)=>{let d=X("div",{"data-ui":"kg-cell",role:"gridcell","aria-colindex":String(l+1),"data-kg-key":a.key,"data-align":a.align});$t.push(d),ae.append(d)});let Be=32,Nt=()=>{A.style.setProperty("--kg-viewport-width",`${G.clientWidth}px`),A.style.setProperty("--kg-pin-start",`${y?Ne:0}px`);let a=X("div",{"data-ui":"kg-row","data-kind":"probe","aria-hidden":"true"});if(a.append(X("div",{"data-ui":"kg-cell"},"M")),ne.append(a),Be=a.getBoundingClientRect().height||Be,a.remove(),!t.height){let l=Math.max(1,t.visibleRows??12),d=Y.getBoundingClientRect().height+(C?q.getBoundingClientRect().height:0);G.style.maxHeight=`${Math.ceil(d+l*Be+2)}px`}},Rt=()=>({scrollTop:G.scrollTop,height:G.clientHeight,header:Y.getBoundingClientRect().height,footer:C?q.getBoundingClientRect().height:0,rowHeight:Be}),S={id:re,key:M[0]?.key??""},h=S,g=null,v=null,T=!1,H=!1,$="",B=new Map,J=new Map,N={start:0,end:0},_=()=>f.view(),yt=-1,dt=new Map,ie=a=>(yt!==f.version()&&(dt=new Map(_().map((l,d)=>[l.id,d])),yt=f.version()),a===re?-1:dt.get(a)??-1),u=a=>Math.max(0,M.findIndex(l=>l.key===a)),L=a=>({row:ie(a.id),col:u(a.key)}),U=a=>({id:a.row<0?re:_()[a.row]?.id??re,key:M[a.col]?.key??M[0]?.key??""}),le=a=>{let l=t.fullWidth?.(a.row)??null;return l&&f.column(l.key)?l:null},Me=(a,l)=>{let d=le(a);return f.column(d?d.key:l)},W=()=>{if(S.id===re)return null;let a=L(h),l=L(S);return a.row<0||l.row<0?ga(l,l):ga(a,l)},Te=()=>({rows:_().length,cols:M.length,header:!0}),vt={danger:Cn("circle-alert"),warning:Cn("triangle-alert")},He=Cn("grip-vertical"),Ft=a=>{let l=X("div",{"data-ui":"kg-row",role:"row"});if(y){let x=X("div",{"data-ui":"kg-handle","aria-hidden":"true",title:n.dragHandle});x.innerHTML=He,l.append(x)}let d=le(a),p=[];if(d){let x=X("div",{"data-ui":"kg-cell",role:"gridcell","aria-colindex":"1","aria-colspan":String(M.length),"data-full-width":d.tone,"data-kg-key":d.key,tabindex:"-1"});x.append(X("span",{"data-ui":"kg-cell-value"})),l.append(x);for(let I=0;I<M.length;I+=1)p.push(x)}else M.forEach((x,I)=>{let O=X("div",{"data-ui":"kg-cell",role:"gridcell","aria-colindex":String(I+1),"data-kg-key":x.key,"data-type":x.type,"data-align":x.align,tabindex:"-1"});x.type==="checkbox"?O.append(X("input",{"data-ui":"kg-check",type:"checkbox",tabindex:"-1"})):O.append(X("span",{"data-ui":"kg-cell-value"})),lt(O,x.key),p.push(O),l.append(O)});return{el:l,cells:p,full:d}},fn=(a,l,d,p,x,I)=>{let O=g?.id===l.id&&g.cell===a,z=d.read(l.row),ee=f.canEdit(l,d),de=f.cellIssue(l,d),be=S.id===l.id&&(le(l)?!0:S.key===d.key),ge=I!==null&&Lr(I,p,x)&&(I.top!==I.bottom||I.left!==I.right);ue(a,"data-editable",ee?"true":null),ue(a,"aria-readonly",K&&!ee?"true":null),ue(a,"data-dirty",f.cellDirty(l,d)?"true":null),ue(a,"data-active",be?"true":null),ue(a,"aria-selected",ge?"true":null),ue(a,"tabindex",be?"0":"-1");let De=O&&v!==null||de?.tone==="danger";if(ue(a,"data-invalid",De?"true":null),ue(a,"aria-invalid",De?"true":null),ue(a,"data-conflict",de?.tone==="warning"?"true":null),ue(a,"aria-describedby",be&&(de||v)?te.id:null),O)return;if(d.type==="checkbox"&&!le(l)){let Oe=a.querySelector('[data-ui="kg-check"]');Oe&&(Oe.checked=z===!0,Oe.disabled=!ee,Oe.setAttribute("aria-label",d.label))}else{let Oe=a.querySelector('[data-ui="kg-cell-value"]');if(Oe){let Ve=d.format(z,l.row),Ue=!Ve&&ee&&d.placeholder?d.placeholder:null,ct=le(l)?.tag,tt=(Ue??Ve)+(ct?` \xB7 ${ct}`:"");Oe.textContent!==tt&&(Oe.textContent=tt),ue(Oe,"data-placeholder",Ue?"true":null)}}let Ee=a.querySelector('[data-ui="kg-flag"]');de?(Ee||(Ee=X("span",{"data-ui":"kg-flag","aria-hidden":"true"}),a.append(Ee)),Ee.dataset.tone!==de.tone&&(Ee.dataset.tone=de.tone,Ee.innerHTML=vt[de.tone])):Ee?.remove()},fe=(a,l,d,p)=>{ue(a.el,"aria-rowindex",String(d+2)),ue(a.el,"data-new",l.base===null?"true":null),ue(a.el,"data-kind",a.full?.tone??null),ue(a.el,"data-removed-elsewhere",l.removedElsewhere?"true":null);let x=a.full?f.column(a.full.key):void 0,I=a.full?!!x&&f.canEdit(l,x):M.every(O=>f.canEdit(l,O));if(ue(a.el,"data-editable",I?"true":null),ue(a.el,"data-unsaved",f.rowUnsaved(l)?"true":null),a.full){let O=le(l)?.tag||null;ue(a.cells[0],"aria-description",O);let z=a.cells[0].querySelector('[data-ui="kg-cell-value"]');z&&ue(z,"data-tag",O),x&&fn(a.cells[0],l,x,d,0,p);return}for(let[O,z]of a.cells.entries())fn(z,l,M[O],d,O,p)},se=()=>{let a=f.sort();st.forEach((l,d)=>{let p=M[d],x=a?.key===p.key?a.direction:null;ue(l,"aria-sort",p.sortable?x==="asc"?"ascending":x==="desc"?"descending":"none":null);let I=l.querySelector('[data-ui="kg-sort"]');if(I){let z=x??"none";I.dataset.direction!==z&&(I.dataset.direction=z,I.innerHTML=x?Cn(x==="asc"?"arrow-up":"arrow-down"):"")}let O=S.id===re&&S.key===p.key;ue(l,"data-active",O?"true":null),ue(l,"tabindex",O?"0":"-1"),lt(l,p.key)})},he=()=>{if(!C)return;let a=f.totals(),l=f.entries().map(d=>d.row);$t.forEach((d,p)=>{let x=M[p],I=a.get(x.key),O=I!==void 0?x.format(I,l[0]??{}):p===0?n.total:"";d.textContent!==O&&(d.textContent=O),lt(d,x.key)}),ue(ae,"aria-rowindex",String(_().length+2))},pe=()=>{let a=f.summary(),l=[];a.added&&l.push({text:Ze(n.addedRows,{count:a.added})}),a.edited&&l.push({text:Ze(n.editedCells,{count:a.edited})}),a.removed&&l.push({text:Ze(n.removedRows,{count:a.removed})}),V.length&&l.push({text:Ze(n.otherPages,{count:V.length})}),a.errors&&l.push({text:Ze(n.errors,{count:a.errors}),tone:"danger"}),a.conflicts&&l.push({text:Ze(n.conflicts,{count:a.conflicts}),tone:"warning"});let d=JSON.stringify(l);Pe.dataset.signature!==d&&(Pe.dataset.signature=d,Pe.replaceChildren(...l.map(z=>X("span",{"data-tone":z.tone??null},z.text))));let p=Ae("undo"),x=Ae("redo"),I=oe.querySelectorAll('button[name="kg-command"][value^="add-"]'),O=Ae("remove");p&&(p.disabled=!f.canUndo()||T),x&&(x.disabled=!f.canRedo()||T);for(let z of I)z.disabled=!f.canInsert()||T;O&&(O.disabled=T||Yt().every(z=>!f.canRemove(z)))},ye=()=>{let a=S.id===re?null:f.entry(S.id),l=a?Me(a,S.key):void 0,d=a&&l?f.cellIssue(a,l):null,p=v??d?.message??(a?.removedElsewhere?n.removedElsewhere:null),x=v?"danger":d?.tone??(a?.removedElsewhere?"warning":null),I=`${x}|${p}|${d?.tone==="warning"?"conflict":""}`;if(te.dataset.signature===I)return;if(te.dataset.signature=I,!p){te.hidden=!0,te.replaceChildren();return}te.hidden=!1,te.dataset.tone=x??"danger";let O=X("span",{"data-ui":"kg-flag","data-tone":x??"danger","aria-hidden":"true"});O.innerHTML=vt[x==="warning"?"warning":"danger"];let z=X("span",{},p);if(te.replaceChildren(O,z),d?.tone==="warning"&&!v){let ee=X("span",{"data-ui":"kg-message-actions"});ee.innerHTML=sn(Gt({label:n.keepMine,variant:"tertiary",name:"kg-resolve",value:"mine"}))+sn(Gt({label:n.takeTheirs,variant:"secondary",name:"kg-resolve",value:"theirs"})),te.append(ee)}},ut=()=>{let a=_().length===0;a&&!xe.isConnected&&ne.insertBefore(xe,ke),!a&&xe.isConnected&&xe.remove()},ft=()=>{let a=_(),l=va(Rt(),a.length),d=a.slice(l.start,l.end),p=new Set(d.map(O=>O.id));for(let[O,z]of B)p.has(O)||(z.el.contains(document.activeElement)&&mn(),z.el.remove(),B.delete(O));let x=W(),I=Q;d.forEach((O,z)=>{let ee=B.get(O.id),de=le(O);ee&&(ee.full?.key??null)!==(de?.key??null)&&(ee.el.remove(),ee=void 0),ee||(ee=Ft(O),B.set(O.id,ee)),I.nextElementSibling!==ee.el&&I.after(ee.el),I=ee.el,fe(ee,O,l.start+z,x)}),Q.style.height=`${l.start*Be}px`,ke.style.height=`${Math.max(0,a.length-l.end)*Be}px`,N=l,ut()},ve=()=>{let a=A.contains(document.activeElement),l=document.activeElement===G;ue(G,"aria-rowcount",String(_().length+1+(C?1:0))),ue(G,"aria-colcount",String(M.length)),se(),ft(),he(),pe(),ye();let d=hn();ue(G,"tabindex",d?"-1":"0"),a&&!g&&(l||!A.contains(document.activeElement))&&d&&d.focus({preventScroll:!0})},mn=()=>{H=!0,G.focus({preventScroll:!0}),H=!1},hn=()=>{if(S.id===re)return st[u(S.key)]??null;let a=B.get(S.id);return a?a.cells[u(S.key)]??null:null},Ln=()=>{let a=f.changes(),l=a.added.length||a.updated.length||a.removed.length||a.order||V.length,d=Object.fromEntries(M.filter(p=>p.learned.size).map(p=>[p.key,Object.fromEntries(p.learned)]));return{...l&&Object.keys(d).length?{optionLabels:d}:{},added:a.added.map(p=>({id:p.id,index:p.index,values:p.row})),updated:[...a.updated.map(p=>({id:p.id,values:Object.fromEntries(p.fields.map(x=>[x,p.row[x]]))})),...V],removed:a.removed,order:a.order}},Zt=a=>{Se.textContent="",requestAnimationFrame(()=>{Se.textContent=a})},Ce=(a,l)=>{if(Re){let d=JSON.stringify(Ln());Re.value!==d&&(Re.value=d,Re.dispatchEvent(new Event("input",{bubbles:!0})),Re.dispatchEvent(new Event("change",{bubbles:!0})))}ve(),l&&Zt(l),a&&t.onChange?.({reason:a.reason,cells:a.cells,changes:f.changes()})},Yr=a=>{for(let l of a){let d=B.get(l.id),p=d?.full?d.cells[0]:d?.cells[u(l.key)];if(!p)continue;let x=`${l.id}\0${l.key}`;window.clearTimeout(J.get(x)),p.dataset.flash="false",p.offsetWidth,p.dataset.flash="true",J.set(x,window.setTimeout(()=>{delete p.dataset.flash,J.delete(x)},1200))}},Ma=()=>{if(S.id!==re){let I=ie(S.id),O=I>=0?Ir(Rt(),I):null;O!==null&&(G.scrollTop=O)}ve();let a=hn();if(!a)return;let l=Fe(),d=xt();if(a.dataset.pinned)return;let p=a.offsetLeft,x=p+a.offsetWidth;p-l<G.scrollLeft?G.scrollLeft=Math.max(0,p-l):x+d>G.scrollLeft+G.clientWidth&&(G.scrollLeft=x+d-G.clientWidth)},qe=()=>{Ma();let a=hn();a?a.focus({preventScroll:!0}):mn();let l=S.id===re?"":`${S.id}\0${S.key}`;l!==$&&($=l,t.onActiveCellChange?.(S.id===re?null:{...S}))},mt=(a,l=!1)=>{let d=U(a);S=d,(!l||d.id===re)&&(h=d),v=null,qe()},Yt=()=>{let a=W();return a?_().slice(a.top,a.bottom+1):[]},Ea=()=>{let a=W();if(!a)return[];let l=[];for(let d of _().slice(a.top,a.bottom+1)){let p=le(d);if(p){let x=f.column(p.key);x&&l.push({entry:d,column:x,col:a.left});continue}for(let x=a.left;x<=a.right;x+=1)l.push({entry:d,column:M[x],col:x})}return l},Sa=(a,l)=>{let d=_()[a];if(!d)return!1;let p=le(d);if(p){let x=f.column(p.key);return l===0&&!!x&&f.canEdit(d,x)}return f.canEdit(d,M[l])},Ta=a=>{let l=S.id===re?{row:a?0:-1,col:M.length-1}:L(S),d=l.row<0?{row:0,col:-1}:l,p=Ar(d,a,Te(),Sa);return p?(mt(p),!0):!a&&f.canInsert()&&S.id!==re?(An(_().length),!0):!1},Qr=a=>{for(let l=0;l<M.length;l+=1)if(Sa(a,l))return l;return 0},An=(a,l=0)=>{f.sort()&&f.setSort(null);let d=E[l]?.create({index:a});if(!d)return;let p=f.insert(a,d);if(!p)return;Ce({reason:"add",cells:[{id:p,key:M[0]?.key??""}]},n.rowAdded);let x=ie(p);mt({row:x,col:Qr(x)})},Ca=()=>{let a=Yt().filter(x=>f.canRemove(x));if(!a.length)return;let l=L(S),d=f.remove(a.map(x=>x.id));if(!d.length)return;Ce({reason:"remove",cells:d.map(x=>({id:x,key:S.key}))},Ze(n.rowsRemoved,{count:d.length}));let p=_().length;mt(p?{row:Math.min(l.row,p-1),col:l.col}:{row:-1,col:l.col})},ei=a=>{if(!y||f.sort())return;let l=Yt();l.length&&f.move(l.map(d=>d.id),{by:a})&&(Ce({reason:"move",cells:[{id:l[0].id,key:S.key}]},n.rowMoved),qe())},Gn=a=>{let l=f.column(a);if(!l?.sortable)return;let d=f.sort(),p=d?.key!==a?{key:a,direction:"asc"}:d.direction==="asc"?{key:a,direction:"desc"}:null;f.setSort(p),ve(),Zt(p?Ze(p.direction==="asc"?n.sortedAscending:n.sortedDescending,{column:l.label}):n.sortCleared)},In=()=>{let a=Ea().filter(({entry:p,column:x})=>x.type==="checkbox"&&f.canEdit(p,x));if(!a.length)return;let l=a.every(({entry:p,column:x})=>x.read(p.row)===!0),d=f.edit(a.map(({entry:p,column:x})=>({id:p.id,key:x.key,value:!l})),"edit");Ce(d.change)},Oa=()=>{let a=Ea().filter(({entry:d,column:p})=>f.canEdit(d,p));if(!a.length)return;let l=f.edit(a.map(({entry:d,column:p})=>({id:d.id,key:p.key,value:Sr(p)})),"clear");Ce(l.change)},ti=()=>{let a=W();if(!a)return;let l=_(),d=a.top===a.bottom?a.top-1:a.top,p=l[d];if(!p||le(p))return;let x=[];for(let I=d+1;I<=a.bottom;I+=1){let O=l[I];if(!(!O||le(O)))for(let z=a.left;z<=a.right;z+=1){let ee=M[z];f.canEdit(O,ee)&&x.push({id:O.id,key:ee.key,value:ee.read(p.row)})}}x.length&&Ce(f.edit(x,"fill").change)},Ht=a=>{if(g)return;let l=a==="undo"?f.undo():f.redo();if(!l)return;Ce(l,a==="undo"?n.undone:n.redone);let d=l.cells.find(p=>ie(p.id)>=0);if(d)S={id:d.id,key:d.key||S.key},h=S;else if(S.id!==re&&ie(S.id)<0){let p=_().length;S=U({row:p?0:-1,col:u(S.key)}),h=S}qe()},ni=a=>a==="number"?n.invalidNumber:a==="date"?n.invalidDate:a==="option"?n.invalidOption:n.invalidValue,et=()=>{let a=g;a&&(g=null,v=null,a.choices?.close(),a.input.remove(),delete a.cell.dataset.editing,delete A.dataset.editing)},Xe=a=>{let l=g;if(!l)return!0;let d=f.entry(l.id),p=f.column(l.key);if(!d||!p)return et(),!0;let x=l.input.value,I,O=null;if(l.choices){let de=l.choices.pick();if(de)I=de.value,O=de.label;else if(x===l.original)I=p.read(d.row);else if(!x.trim())I=null;else{let be=ma(x,[...p.choices(d.row),...l.choices.results()]);if(!be)return v=n.invalidOption,l.input.setAttribute("aria-invalid","true"),ve(),!1;I=be.value,O=be.label}}else{let de=p.parse(x,d.row);if(!de.ok)return v=ni(de.reason),l.input.setAttribute("aria-invalid","true"),l.input.setAttribute("aria-describedby",te.id),ve(),!1;I=de.value}O&&I!==null&&p.learned.set(String(I),O),a.quiet&&mn(),et();let z=p.read(d.row),ee=gt(z,I)&&x===l.original?null:f.edit([{id:d.id,key:p.key,value:I}],"edit").change;return Ce(ee),a.quiet||(a.tab?Ta(a.backwards===!0)||qe():a.rows?mt(ln(L(S),a.rows,0,Te())):qe()),!0},La=()=>{et(),ve(),qe()},Mt=a=>{if(g||T||S.id===re)return!1;let l=f.entry(S.id),d=l?Me(l,S.key):void 0;if(!l||!d)return!1;if(!f.canEdit(l,d))return Zt(n.readOnlyCell),!1;if(d.type==="checkbox")return In(),!1;Ma();let p=hn();if(!p)return!1;let x=d.read(l.row),I=kr(x,d.spec),O=X("input",{"data-ui":"kg-editor",type:d.type==="date"?"date":"text","aria-label":d.label,autocomplete:"off",spellcheck:d.type==="text"?null:"false",inputmode:d.type==="number"||d.type==="percent"||d.type==="money"?"decimal":null,"data-align":d.align});return O.value=a==="replace"?"":I,p.dataset.editing="true",A.dataset.editing="true",p.append(O),g={id:l.id,key:d.key,input:O,cell:p,mode:a,original:I,choices:null},(d.type==="select"||d.type==="combobox")&&(g.choices=ri(O,p,l,d,a==="edit"?I:"")),O.focus({preventScroll:!0}),a==="edit"&&O.type==="text"&&O.setSelectionRange(O.value.length,O.value.length),O.addEventListener("keydown",z=>ai(z),{signal:c}),O.addEventListener("input",()=>{v&&(v=null,O.removeAttribute("aria-invalid"),ve())},{signal:c}),!0},ai=a=>{let l=g;if(!l||a.isComposing)return;if(l.choices?.handleKey(a)){a.preventDefault(),a.stopPropagation();return}let d=l.input.type==="text"&&!l.choices,p=()=>{a.preventDefault(),a.stopPropagation()};switch(a.key){case"Enter":p(),Xe({rows:a.shiftKey?-1:1});return;case"Tab":p(),Xe({tab:!0,backwards:a.shiftKey});return;case"Escape":p(),La();return;case"ArrowUp":case"ArrowDown":if(l.input.type!=="text")return;p(),Xe({rows:a.key==="ArrowUp"?-1:1});return;case"ArrowLeft":case"ArrowRight":{if(!d||l.mode!=="replace"){a.stopPropagation();return}p(),Xe({})&&mt(ln(L(S),0,a.key==="ArrowLeft"?-1:1,Te()));return}default:a.stopPropagation()}},Kn=typeof HTMLElement<"u"&&"showPopover"in HTMLElement.prototype,ri=(a,l,d,p,x)=>{let I=`${s}-options`,O=X("div",{"data-ui":"kg-popup"});Kn&&O.setAttribute("popover","manual");let z=X("div",{"data-ui":"kg-options-status",role:"status",hidden:""}),ee=X("div",{"data-ui":"kg-options",role:"listbox",id:I,"aria-label":p.label});O.append(z,ee),A.append(O),a.setAttribute("role","combobox"),a.setAttribute("aria-autocomplete","list"),a.setAttribute("aria-controls",I),a.setAttribute("aria-expanded","true");let de=[],be=-1,ge=null,De=!1,Ee=0,Oe=null,Ve=p.read(d.row),Ue=()=>{ee.replaceChildren(...de.map((ce,Ge)=>{let ze=X("div",{"data-ui":"kg-option",role:"option",id:`${I}-${Ge}`,"aria-selected":Ge===be?"true":"false","data-current":!Ie(Ve)&&ce.value===String(Ve)?"true":null});return ze.append(X("span",{"data-ui":"kg-option-label"},ce.label)),ce.description&&ze.append(X("span",{"data-ui":"kg-option-description"},ce.description)),ze})),ue(a,"aria-activedescendant",be>=0?`${I}-${be}`:null),ee.children[be]?.scrollIntoView({block:"nearest"})},ct=ce=>{z.hidden=ce===null,z.textContent=ce??""},tt=(ce,Ge)=>{de=ce;let ze=ce.findIndex(We=>We.value===String(Ve));be=Ge?ce.length?0:-1:ze,ct(ce.length?null:n.noOptions),Ue(),wt()},pn=()=>{let ce=De?a.value.trim():"";if(p.search){window.clearTimeout(Ee),Oe?.abort(),ct(n.searching),Ee=window.setTimeout(()=>{let We=new AbortController;Oe=We,p.search?.(ce,{row:d.row,signal:We.signal}).then(bn=>{if(!We.signal.aborted){for(let yn of bn)p.learned.set(yn.value,yn.label);tt(bn,ce)}}).catch(()=>{We.signal.aborted||(de=[],be=-1,Ue(),ct(n.searchFailed))})},ce?200:0);return}let Ge=Ke(ce),ze=p.choices(d.row);tt(Ge?ze.filter(We=>Ke(We.label).includes(Ge)||Ke(We.description??"").includes(Ge)):ze,ce)},wt=()=>{if(!O.isConnected)return;let ce=l.getBoundingClientRect(),Ge=Math.max(ce.width,240);O.style.width=`${Ge}px`,O.style.left=`${Math.max(8,Math.min(ce.left,window.innerWidth-Ge-8))}px`;let ze=O.getBoundingClientRect().height,We=window.innerHeight-ce.bottom-8,bn=ce.top-8,yn=We>=ze||We>=bn?ce.bottom+2:ce.top-2-ze;O.style.top=`${Math.max(8,yn)}px`};Kn&&O.showPopover(),a.addEventListener("input",()=>{De=a.value!==x,ge=null,pn()},{signal:c}),ee.addEventListener("pointerdown",ce=>{let Ge=ce.target.closest('[data-ui="kg-option"]');if(!Ge)return;ce.preventDefault();let ze=[...ee.children].indexOf(Ge);ge=de[ze]??null,ge&&Xe({})},{signal:c});let Et=()=>wt();return window.addEventListener("resize",Et,{signal:c}),G.addEventListener("scroll",Et,{signal:c,passive:!0}),pn(),{pick:()=>ge??(De&&be>=0?de[be]??null:null),results:()=>de,handleKey:ce=>{if(ce.key==="ArrowDown"||ce.key==="ArrowUp"){if(!de.length)return!0;let Ge=ce.key==="ArrowDown"?1:-1;return be=(be+Ge+de.length)%de.length,De=!0,ge=null,Ue(),!0}return ce.key==="Enter"&&be>=0&&!ge&&(ge=de[be]??null),!1},place:wt,close:()=>{window.clearTimeout(Ee),Oe?.abort(),window.removeEventListener("resize",Et),G.removeEventListener("scroll",Et),Kn&&O.matches(":popover-open")&&O.hidePopover(),O.remove()}}},Aa=()=>{let a=W();if(!a)return null;let l=[],d=0;for(let p of _().slice(a.top,a.bottom+1)){let x=le(p),I=[];for(let O=a.left;O<=a.right;O+=1){let z=x?f.column(x.key):M[O];I.push(z&&(!x||O===a.left)?z.format(z.read(p.row),p.row):""),d+=1}l.push(I)}return{text:dr(l),count:d}},ii=a=>{let l=cr(a),d=W();if(!d||!l.length)return;let p=0,x=0,O=l.length===1&&l[0].length===1&&(d.top!==d.bottom||d.left!==d.right),z=O?d.bottom-d.top+1:l.length,ee=O?d.right-d.left+1:Math.max(...l.map(ge=>ge.length)),{change:de}=f.batch("paste",()=>{let ge=d.top+z-_().length;if(ge>0&&f.canInsert()&&!f.sort())for(let Ve=0;Ve<ge;Ve+=1)f.insert(f.entries().length);let De=_(),Ee=[];for(let Ve=0;Ve<z;Ve+=1){let Ue=De[d.top+Ve];if(!Ue){x+=ee;continue}let ct=le(Ue);for(let tt=0;tt<ee;tt+=1){let pn=O?l[0][0]:l[Ve]?.[tt]??"",wt=ct?tt===0?f.column(ct.key):void 0:M[d.left+tt];if(!wt){ct||(x+=1);continue}if(!f.canEdit(Ue,wt)){x+=1;continue}let Et=wt.parse(pn,Ue.row);if(!Et.ok){x+=1;continue}Ee.push({id:Ue.id,key:wt.key,value:Et.value})}}let Oe=f.edit(Ee,"paste");p=Ee.length-Oe.skipped,x+=Oe.skipped}),be=[Ze(n.pasted,{count:p})];x&&be.push(Ze(n.pasteSkipped,{count:x})),Ce(de,be.join(". ")),h=U({row:d.top,col:d.left}),S=U({row:Math.min(_().length-1,d.top+z-1),col:Math.min(M.length-1,d.left+ee-1)}),qe()},oi=(a,l)=>{let d=L(S),p=_();switch(a.type){case"move":return mt(ln(d,a.rows,a.cols,Te()),a.extend),!0;case"edge":{if(S.id===re&&a.rows===0)return mt(ha(d,0,a.cols,Te())),!0;let x=ha(d.row<0?{...d,row:0}:d,a.rows,a.cols,Te());return p.length||(x.row=-1),mt(x,a.extend),!0}case"page":{let x=Kr(Rt())*a.direction;return mt(ln(d.row<0?{...d,row:0}:d,x,0,Te()),a.extend),!0}case"tab":return Ta(a.backwards);case"edit":return S.id===re?Gn(S.key):Mt("edit"),!0;case"type":{if(S.id===re)return!1;let x=f.entry(S.id),I=x?Me(x,S.key):void 0;return!I||I.type==="checkbox"||I.type==="date"?(I?.type==="date"&&Mt("edit"),!0):(Mt("replace"),!1)}case"toggle":{let x=S.id===re?void 0:f.entry(S.id),I=x?Me(x,S.key):void 0;return S.id===re?(Gn(S.key),!0):I?.type==="checkbox"?(In(),!0):I&&f.canEdit(x,I)?(Mt("replace"),!1):!0}case"clear":return Oa(),!0;case"selectAll":return p.length&&(h=U({row:0,col:0}),S=U({row:p.length-1,col:M.length-1}),ve(),qe()),!0;case"fillDown":return ti(),!0;case"undo":return Ht("undo"),!0;case"redo":return Ht("redo"),!0;case"moveRows":return ei(a.direction),!0;case"insertRow":return f.canInsert()?(An(S.id===re?0:d.row+1),!0):!1;case"removeRows":return Ca(),!0;case"collapse":return h.id===S.id&&h.key===S.key?!1:(h=S,ve(),!0)}};G.addEventListener("keydown",a=>{if(g||a.defaultPrevented||a.target.closest('[data-ui="kg-editor"]'))return;let l=Or(a);l&&(T&&l.type!=="move"&&l.type!=="edge"&&l.type!=="page"||oi(l,a)&&a.preventDefault())},{signal:c}),G.addEventListener("focus",()=>{!g&&!H&&qe()},{signal:c}),A.addEventListener("focusin",a=>{let l=a.target.closest('[data-ui="kg-cell"], [data-ui="kg-header-cell"]');if(!l||!G.contains(l)||g)return;let d=Pn(l);d&&(d.id!==S.id||d.key!==S.key)&&(S=d,h=d,ve())},{signal:c}),A.addEventListener("focusout",a=>{if(!g)return;let l=a.relatedTarget;l&&A.contains(l)||window.setTimeout(()=>{g&&!A.contains(document.activeElement)&&(Xe({})||La())},0)},{signal:c}),window.addEventListener("pointerdown",a=>{!g||a.target instanceof Node&&A.contains(a.target)||Xe({})},{capture:!0,signal:c});let Pn=a=>{if(a.dataset.ui==="kg-header-cell")return{id:re,key:a.dataset.kgKey??""};if(q.contains(a))return null;for(let[l,d]of B){let p=d.cells.indexOf(a);if(p>=0)return{id:l,key:d.full&&S.id===l?S.key:M[p].key}}return null},Dt=null;G.addEventListener("pointerdown",a=>{if(a.button!==0)return;let l=a.target;if(l.closest('[data-ui="kg-editor"]'))return;if(l.closest('[data-ui="kg-resize"]'))return li(a);if(l.closest('[data-ui="kg-handle"]'))return si(a);let d=l.closest('[data-ui="kg-cell"], [data-ui="kg-header-cell"]');if(!d||q.contains(d))return;let p=Pn(d);if(!p)return;if(g&&!Xe({})){a.preventDefault();return}let x=p.id===S.id&&p.key===S.key;a.shiftKey&&p.id!==re?S=p:(S=p,h=p),v=null,a.pointerType==="mouse"&&a.preventDefault(),qe(),Dt={pointerId:a.pointerId,moved:!1,wasActive:x}},{signal:c}),G.addEventListener("pointermove",a=>{if(!Dt||Dt.pointerId!==a.pointerId||!(a.buttons&1)||S.id===re||a.pointerType!=="mouse")return;let l=document.elementFromPoint(a.clientX,a.clientY)?.closest('[data-ui="kg-cell"]');if(!l||!ne.contains(l))return;let d=Pn(l);!d||d.id===S.id&&d.key===S.key||(Dt.moved=!0,S=d,ve())},{signal:c}),G.addEventListener("pointerup",a=>{let l=Dt;Dt=null,!(!l||l.pointerId!==a.pointerId||l.moved||!l.wasActive||a.shiftKey||a.target.closest('[data-ui="kg-check"]'))&&S.id!==re&&Mt("edit")},{signal:c}),G.addEventListener("dblclick",a=>{a.target.closest('[data-ui="kg-editor"], [data-ui="kg-check"], [data-ui="kg-resize"]')||S.id!==re&&!g&&Mt("edit")},{signal:c}),G.addEventListener("click",a=>{let l=a.target;if(l.closest('[data-ui="kg-check"]')){In(),ve();return}let d=l.closest('[data-ui="kg-header-cell"]');d&&!l.closest('[data-ui="kg-resize"]')&&!$n&&Gn(d.dataset.kgKey??"")},{signal:c});let $n=!1,li=a=>{let l=a.target.closest('[data-ui="kg-header-cell"]'),d=l?f.column(l.dataset.kgKey??""):void 0;if(!l||!d)return;a.preventDefault(),a.stopPropagation();let p=a.target;p.setPointerCapture(a.pointerId);let x=a.clientX,I=l.getBoundingClientRect().width;A.dataset.resizing="true";let O=ee=>{it.set(d.key,Math.max(d.minWidth,Math.round(I+ee.clientX-x))),je()},z=()=>{p.removeEventListener("pointermove",O),delete A.dataset.resizing,$n=!0,window.setTimeout(()=>{$n=!1},0)};p.addEventListener("pointermove",O,{signal:c}),p.addEventListener("pointerup",z,{once:!0,signal:c}),p.addEventListener("pointercancel",z,{once:!0,signal:c})};Y.addEventListener("dblclick",a=>{let l=a.target.closest('[data-ui="kg-header-cell"]');!l||!a.target.closest('[data-ui="kg-resize"]')||(it.delete(l.dataset.kgKey??""),je())},{signal:c});let si=a=>{if(!y||f.sort()||T)return;let l=a.target.closest('[data-ui="kg-handle"]'),d=l?.closest('[data-ui="kg-row"]'),p=[...B].find(([,ge])=>ge.el===d)?.[0];if(!l||!p||(a.preventDefault(),g&&!Xe({})))return;let x=Yt().some(ge=>ge.id===p)?Yt().map(ge=>ge.id):[p];x.includes(S.id)||(S={id:p,key:S.key===""?M[0]?.key??"":S.key},h=S),l.setPointerCapture(a.pointerId),A.dataset.dragging="true";let I=ie(x[0]),O=0,z=a.clientY,ee=()=>{let ge=G.getBoundingClientRect(),De=32;z<ge.top+De?G.scrollTop-=8:z>ge.bottom-De&&(G.scrollTop+=8);let Ee=ne.getBoundingClientRect().top;I=Math.max(0,Math.min(_().length,Math.round((z-Ee)/Be))),F.hidden=!1,F.style.top=`${I*Be}px`,O=requestAnimationFrame(ee)};O=requestAnimationFrame(ee);let de=ge=>{z=ge.clientY},be=()=>{cancelAnimationFrame(O),l.removeEventListener("pointermove",de),F.hidden=!0,delete A.dataset.dragging;let ge=Math.min(...x.map(Ee=>f.entries().findIndex(Oe=>Oe.id===Ee))),De=I>ge?I-x.length:I;f.move(x,{to:De})&&Ce({reason:"move",cells:[{id:x[0],key:S.key}]},n.rowMoved),qe()};l.addEventListener("pointermove",de,{signal:c}),l.addEventListener("pointerup",be,{once:!0,signal:c}),l.addEventListener("pointercancel",be,{once:!0,signal:c})};A.addEventListener("copy",a=>{if(g||!G.contains(a.target))return;let l=Aa();!l||!a.clipboardData||(a.preventDefault(),a.clipboardData.setData("text/plain",l.text),Zt(Ze(n.copied,{count:l.count})))},{signal:c}),A.addEventListener("cut",a=>{if(g||!G.contains(a.target)||T)return;let l=Aa();!l||!a.clipboardData||(a.preventDefault(),a.clipboardData.setData("text/plain",l.text),Oa())},{signal:c}),A.addEventListener("paste",a=>{if(g||!G.contains(a.target)||T||S.id===re)return;let l=a.clipboardData?.getData("text/plain");l&&(a.preventDefault(),ii(l))},{signal:c}),oe.addEventListener("click",a=>{let l=a.target.closest('button[name="kg-command"]');!l||l.disabled||g&&!Xe({})||(l.value.startsWith("add-")?(An(_().length,Number(l.value.slice(4))),Mt("edit")):l.value==="remove"?Ca():l.value==="undo"?Ht("undo"):l.value==="redo"&&Ht("redo"))},{signal:c}),te.addEventListener("click",a=>{let l=a.target.closest('button[name="kg-resolve"]');if(!l||S.id===re)return;let d=f.entry(S.id),p=d?Me(d,S.key):void 0;p&&(Ce(f.resolve(S.id,p.key,l.value==="theirs"?"theirs":"mine")),qe())},{signal:c});let gn=0;G.addEventListener("scroll",()=>{gn||(gn=requestAnimationFrame(()=>{gn=0;let a=va(Rt(),_().length);if(a.start===N.start&&a.end===N.end)return;let l=g?ie(g.id):-1;g&&(l<a.start||l>=a.end)&&!Xe({quiet:!0})&&(mn(),et()),ve()}))},{signal:c,passive:!0}),G.addEventListener("pointerover",a=>{let l=a.target.closest('[data-ui="kg-cell-value"]');if(!l)return;let d=l.parentElement;d&&ue(d,"title",l.scrollWidth>l.clientWidth?l.textContent:null)},{signal:c});let Ga=typeof ResizeObserver>"u"?null:new ResizeObserver(()=>{Nt(),ve()});Ga?.observe(G),(typeof matchMedia>"u"?null:matchMedia("(max-width: 47.9375rem)"))?.addEventListener("change",()=>{Nt(),ve()},{signal:c}),e.append(A),je(),Nt(),Re&&(Re.defaultValue=Pr,Re.value=JSON.stringify(Ln()),window.addEventListener("submit",a=>{let l=Re.form;if(!l||a.target!==l||l.noValidate)return;let d=a.submitter;if((d instanceof HTMLButtonElement||d instanceof HTMLInputElement)&&d.formNoValidate)return;let p=g!==null&&!Xe({}),x=f.revealIssues();ve(),!(!p&&!x.length)&&(a.preventDefault(),a.stopImmediatePropagation(),!p&&x[0]&&Ia.focusCell(x[0].id,x[0].key),Zt(Ze(n.fixBeforeSaving,{count:Math.max(x.length,1)})))},{capture:!0,signal:c})),ve();let Ia={element:A,rows:()=>f.entries().map(a=>a.row),changes:()=>f.changes(),posted:Ln,summary:()=>f.summary(),dirty:()=>{let a=f.summary();return a.added+a.edited+a.removed+V.length>0||f.changes().order!==null},issues:()=>f.issues(),validate:()=>{let a=f.revealIssues();return ve(),a},setReadOnly:a=>{a!==(t.readOnly===!0)&&(et(),t.readOnly=a,f.setReadOnly(a),K=!a&&(M.some(l=>!l.computed||l.computedWriter)||E.length>0||b!==void 0),ue(A,"data-editable",K?"true":"false"),ue(G,"aria-readonly",K?null:"true"),ve())},setRows:a=>{et(),V=[],f.reset(a),S.id!==re&&ie(S.id)<0&&(S={id:re,key:S.key},h=S),Ce(null)},applyTransaction:a=>{let l=g?{id:g.id,key:g.key}:null,d=f.apply(a);l&&!f.entry(l.id)&&et(),S.id!==re&&ie(S.id)<0&&(S={id:re,key:S.key},h=S),Ce(null),Yr(d.changed.filter(p=>!(l&&p.id===l.id&&p.key===l.key)))},markSaved:a=>{et(),V=[],f.markSaved(a),S.id!==re&&ie(S.id)<0&&(S={id:re,key:S.key},h=S),Ce(null)},revert:()=>{et(),V=[],f.revert(),S.id!==re&&ie(S.id)<0&&(S={id:re,key:S.key},h=S),Ce(null)},setErrors:a=>{f.setErrors(a),ve()},patch:(a,l)=>Ce(f.patchRow(a,l)),addRow:(a,l)=>{let d=f.insert(l??f.entries().length,a);return d&&Ce({reason:"add",cells:[{id:d,key:M[0]?.key??""}]}),d},removeRows:a=>{let l=f.remove(a);return l.length&&Ce({reason:"remove",cells:l.map(d=>({id:d,key:S.key}))}),l},focusCell:(a,l)=>{ie(a)<0||!f.column(l)||(S={id:a,key:l},h=S,qe())},setBusy:a=>{T=a,T&&et(),ue(A,"aria-busy",T?"true":null),ue(A,"data-busy",T?"true":null),ve()},undo:()=>Ht("undo"),redo:()=>Ht("redo"),destroy:()=>{et(),m.abort(),Ga?.disconnect();for(let a of J.values())window.clearTimeout(a);cancelAnimationFrame(gn),A.remove()}};return Ia}var no=(e,t)=>async(n,o)=>{let i={...e.listInput??{}};for(let[y,b]of Object.entries(e.listInputFields??{})){let w=o.row[b];if(w==null||w==="")return[];i[y]=w}i[e.searchParam??"search"]=n,i[e.limitParam??"limit"]=e.limit??50;let s=await fetch(`/_ket/fn/${encodeURIComponent(e.listFunction)}`,{method:"POST",credentials:"same-origin",headers:{"content-type":"application/json"},body:JSON.stringify(i),signal:o.signal}),m=await s.json();if(!s.ok||m.ok===!1)throw new Error(String(m.errors?.[0]?.message??m.message??`HTTP ${s.status}`));let c=Array.isArray(m.value)?m.value:[];return t?.(c,o.row),c.map(y=>({value:String(y[e.idField??"id"]??""),label:String(y[e.labelField??"name"]||(e.labelFallbackField?y[e.labelFallbackField]:"")||""),description:e.descriptionField&&String(y[e.descriptionField]??"")||null}))},ao=e=>{let t=e;if(typeof e=="string")try{t=e?JSON.parse(e):null}catch{return null}let n=t?.updated;if(!Array.isArray(n))return null;let o=t.optionLabels,i=o&&typeof o=="object"&&!Array.isArray(o)?Object.fromEntries(Object.entries(o).filter(([,s])=>s&&typeof s=="object"&&!Array.isArray(s)).map(([s,m])=>[s,Object.fromEntries(Object.entries(m).filter(([,c])=>typeof c=="string"))])):void 0;return{...i?{optionLabels:i}:{},added:Array.isArray(t.added)?t.added.filter(s=>typeof s?.id=="string"&&Number.isInteger(s.index)&&s.index>=0&&!!s.values&&typeof s.values=="object"&&!Array.isArray(s.values)):[],updated:n.filter(s=>typeof s?.id=="string"&&!!s.values&&typeof s.values=="object"&&!Array.isArray(s.values)),removed:Array.isArray(t.removed)?t.removed.filter(s=>typeof s=="string"):[],order:Array.isArray(t.order)?t.order.filter(s=>typeof s=="string"):null}},cn=e=>e!=null&&e!==!1&&e!==0&&e!=="",ro=e=>{let t=e.idField??"id",n=b=>e.lockedField?b[e.lockedField]===!0:!1,o=e.labels?.belowMin??dn.belowMin,i=e.labels?.aboveMax??dn.aboveMax,s=new Intl.NumberFormat(e.locale),m=e.tiers,c=typeof m?.lockedBy=="string"?[m.lockedBy]:m?.lockedBy??[],y=b=>{if(!m||c.some(f=>cn(b[f])))return b;let w=b[m.by];if(typeof w!="number")return b;let E,k=String(b[m.key]??""),M=m.with?m.steps?.[`${k}|${String(b[m.with]??"")}`]:void 0,C=m.stepsField?b[m.stepsField]:void 0,K=Array.isArray(C)?C.filter(f=>!!f&&typeof f.from=="number"&&Number.isFinite(f.from)&&f.row&&typeof f.row=="object"&&!Array.isArray(f.row)):M??m.steps?.[k]??[];for(let f of K)f.from<=w&&(!E||f.from>=E.from)&&(E=f);return E?{...b,...E.row}:b};return{label:e.label,rows:e.rows,rowId:b=>String(b[t]??""),labels:e.labels,locale:e.locale,currency:e.currency,density:e.density,visibleRows:e.visibleRows,height:e.height,readOnly:e.readOnly,rowEditable:b=>!n(b),createRow:e.newRow?()=>({...e.newRow}):void 0,newRows:e.newRows?.map(b=>({label:b.label,create:()=>({...b.row})})),removable:e.removable?b=>!n(b):void 0,reorderable:e.reorderable,fullWidth:e.kindField&&e.fullWidthKey?b=>{let w=b[e.kindField??""];return w==="section"||w==="note"?{key:e.fullWidthKey??"",tone:w,tag:e.fullWidthTag&&cn(b[e.fullWidthTag.field])?e.fullWidthTag.label:void 0}:null}:void 0,name:e.name,form:e.form,carry:ao(e.carry),draftFlags:e.draftFlags?.filter(b=>b!==e.lockedField&&!c.includes(b)&&!e.columns.some(w=>w.lockedBy===b)),columns:e.columns.map(({compute:b,search:w,lockedBy:E,requires:k,minField:M,maxField:C,sumEquals:K,sets:f,resets:V,writeSets:A,optionsBy:j,...G})=>{let Y=G.field??G.key,D=new Map,ne=(F,q)=>JSON.stringify([...Object.values(w?.listInputFields??{}).map(ae=>F[ae]??null),q]),Q=(F,q)=>{if(w?.setsFrom)for(let ae of F)D.set(ne(q,String(ae[w.idField??"id"]??"")),Object.fromEntries(Object.entries(w.setsFrom).map(([te,oe])=>[te,ae[oe]??null]))),D.size>1e3&&D.delete(D.keys().next().value)},ke=b&&"multiply"in b?b.solve:void 0,xe=ke&&e.columns.some(F=>(F.field??F.key)===ke.field&&F.type==="text"),me=f||A||V?.length||w?.setsFrom?(F,q)=>({...F,...F[Y]!==q?Object.fromEntries((V??[]).filter(ae=>{let te=e.columns.find(oe=>(oe.field??oe.key)===ae);return!te?.lockedBy||!cn(F[te.lockedBy])}).map(ae=>[ae,null])):{},[Y]:q,...f?.[String(q)]??{},...D.get(ne(F,q))??{},...A}):void 0;return{...G,value:b?F=>Wa(b,F):void 0,search:w?async(F,q)=>{let ae=j?.values[String(q.row[j.field]??"")];if(!ae)return no(w,Q)(F,q);let te=Ke(F);return(G.options??[]).filter(oe=>ae.includes(oe.value)&&Ke(`${oe.label} ${oe.description??""}`).includes(te))}:void 0,optionsFor:j?F=>{let q=j.values[String(F[j.field]??"")],ae=G.options??[];return q?ae.filter(te=>q.includes(te.value)):ae}:void 0,editable:G.editable&&(E||k||ke)?F=>(!E||!cn(F[E]))&&(!k||cn(F[k]))&&(!ke||oa(b,F)):G.editable,set:ke?(F,q)=>{let ae=Za(b,F,q,!!xe);return ae===F?F:{...ae,...A}}:m&&(Y===m.key||Y===m.by||Y===m.with)?(F,q)=>y(me?me(F,q):{...F,[Y]:q}):me,validate:M||C||K?(F,q)=>{if(K){let oe=Number(q[K.total]),Pe=K.fields.reduce((Se,Re)=>Se+Number(q[Re]??0),0);if(!Number.isFinite(oe)||!Number.isFinite(Pe)||Math.abs(Pe-oe)>1e-6)return K.message}if(typeof F!="number")return null;let ae=M?Number(q[M]??0):Number.NaN;if(Number.isFinite(ae)&&F<ae)return o.replace("{min}",s.format(ae));let te=C?Number(q[C]??Number.NaN):Number.NaN;return Number.isFinite(te)&&F>te?i.replace("{max}",s.format(te)):null}:void 0}})}},io=e=>r("div",{"data-ui":"kg-placeholder","aria-busy":"true",children:r(or,{label:e.label??dn.loading,lines:Math.max(2,Math.min(12,Math.trunc(e.rows??4)))})}),$r=e=>{let{id:t,config:n}=e,o=null,i=JSON.stringify(n.rows);return{view:()=>r("div",{"data-ui":"ket-grid-island",id:t,children:r(io,{label:n.labels?.loading,rows:Math.min(n.rows.length||4,8)})}),mount:({root:s})=>{let m=s.querySelector('[data-ui="ket-grid-island"]');if(!m)return;let c=m.querySelector('[data-ui="kg-placeholder"]');c&&(c.hidden=!0),o=wa(m,ro(n)),m.dispatchEvent(new CustomEvent("ket:grid-ready",{bubbles:!0,detail:{grid:o}}))},update:s=>{let m=s.config;o?.setReadOnly(m.readOnly===!0);let c=JSON.stringify(m.rows);o&&c!==i&&o.setRows(m.rows),i=c},dispose:()=>{o?.destroy(),o=null}}};var Nr=({anchor:e,viewport:t,width:n,height:o,gap:i,inset:s,rtl:m})=>{let c=Math.ceil(t.left+s),y=Math.floor(t.left+t.width-s),b=Math.ceil(t.top+s),w=Math.floor(t.top+t.height-s),E=Math.max(0,Math.min(Math.floor(n),y-c)),k=Math.max(0,w-Math.round(e.bottom+i)),M=Math.max(0,Math.round(e.top-i)-b),C=o<=k||k>=M,K=C?k:M;return{left:Math.max(c,Math.min(Math.round(m?e.right-E:e.left),y-E)),top:C?Math.max(b,Math.round(e.bottom+i)):Math.max(b,Math.round(e.top-i-Math.min(o,K))),width:E,maxHeight:K}};var Fr=(e,t,n,o)=>!e?.isConnected||t&&t!==n&&t.isConnected&&!o?.contains(t)?!1:(e.focus({preventScroll:!0}),!0);var rt=(e,t,n)=>[t?`${e}-help`:null,n?`${e}-error`:null].filter(Boolean).join(" ")||null,ka=e=>R(P,{children:[!!e.help&&r("small",{"data-ui":"field-help",id:`${e.id}-help`,children:r(ht,{children:e.help})}),!!e.error&&r("small",{"data-ui":"field-error",id:`${e.id}-error`,children:r(ht,{children:e.error})})]}),Kt=e=>{let t=e.group?"span":"label";return R("div",{"data-ui":"field","data-kind":e.kind,"data-layout":e.layout??null,"data-size":e.size??null,"data-span":e.span??"half","data-width":e.width??null,"data-label-hidden":e.labelHidden?"true":null,"data-selection-hidden":e.selectionHidden?"true":null,"data-invalid":String(!!e.error),children:[R(t,{"data-ui":"field-label",id:e.group?`${e.id}-label`:void 0,for:e.group?null:e.id,children:[r(ht,{children:e.label}),e.required&&r("span",{"data-ui":"field-required","aria-hidden":"true",children:" *"})]}),e.action===void 0?e.control:R("div",{"data-ui":"field-row",children:[e.control,e.action]}),r(ka,{...e})]})};var Hr=(e,t)=>e?.find(n=>n.path===t)?.message??null;var oo=/^(-?)(\d+)(?:\.(\d+))?$/u,Dr=new Map,lo=e=>{let t=Dr.get(e);if(t)return t;let n=[];try{n=new Intl.NumberFormat(e).formatToParts(12345675e-1)}catch{n=new Intl.NumberFormat("en").formatToParts(12345675e-1)}let o={group:n.find(i=>i.type==="group")?.value??",",decimal:n.find(i=>i.type==="decimal")?.value??"."};return Dr.set(e,o),o};var Vr=(e,t)=>{let n=String(e??"").trim(),o=oo.exec(n);if(!o)return n;let{group:i,decimal:s}=lo(t),m=(o[2]??"").replace(/\B(?=(\d{3})+(?!\d))/gu,i);return`${o[1]}${m}${o[3]?`${s}${o[3]}`:""}`};var so=(e,t)=>{if(e.control!==void 0)return r(P,{children:e.control});let n=!!e.numberLocale&&(e.type==="number"||e.type==="decimal");if(e.type==="textarea")return r("textarea",{"on:input":e.onInput,"on:change":e.onChange,"on:blur":e.onBlur,"data-ui":"field-control","data-appearance":e.appearance==="embedded"?"embedded":null,id:e.id,name:e.name,rows:e.rows,"data-resize":e.resize??null,maxlength:e.maxLength,minlength:e.minLength,placeholder:e.placeholder??null,required:e.required===!0,disabled:e.disabled===!0,readonly:e.readOnly===!0,"aria-invalid":e.error?"true":null,"aria-describedby":t,children:String(e.value??"")});if(e.type==="select"){let o=e.options??[],i=String(e.value??""),s=o.some(c=>c.value===i),m=s||e.placeholder?null:o.find(c=>c.disabled!==!0)?.value??null;return R("select",{"on:input":e.onInput,"on:change":e.onChange,"on:blur":e.onBlur,"data-ui":"field-control","data-appearance":e.appearance==="embedded"?"embedded":null,id:e.id,name:e.name,required:e.required===!0,disabled:e.disabled===!0,"aria-invalid":e.error?"true":null,"aria-describedby":t,children:[e.placeholder&&r("option",{value:"",disabled:e.required===!0,selected:!s,children:e.placeholder}),we(o,c=>c.value,c=>r("option",{value:c.value,selected:i===c.value||m===c.value,disabled:c.disabled===!0,children:c.label}))]})}return r("input",{"on:input":e.onInput,"on:change":e.onChange,"on:blur":e.onBlur,"data-ui":"field-control","data-appearance":e.appearance==="embedded"?"embedded":null,id:e.id,type:n?"text":e.type==="decimal"?"number":e.type??"text",name:e.name,value:e.type==="checkbox"?e.submitValue??"1":n?Vr(e.value,e.numberLocale):String(e.value??""),"data-number-locale":n?e.numberLocale:null,checked:e.type==="checkbox"&&(e.checked===void 0?e.value===!0||e.value==="1":e.checked===!0),"data-indeterminate":e.type==="checkbox"&&e.checked==="indeterminate"?"true":null,"aria-checked":e.type==="checkbox"&&e.checked==="indeterminate"?"mixed":null,inputmode:n?"decimal":e.inputMode??null,"data-align":e.align??null,maxlength:e.maxLength,minlength:e.minLength,pattern:e.pattern??null,placeholder:e.placeholder??null,required:e.required===!0,disabled:e.disabled===!0,readonly:e.readOnly===!0,min:n?null:e.min,max:n?null:e.max,"aria-invalid":e.error?"true":null,"aria-describedby":t,autocomplete:e.autocomplete??"off",step:n?null:e.type==="decimal"?e.step??"any":e.step??null})},Ut=(e,t)=>{if(e.control!==void 0)return r(P,{children:e.control});let n=so(e,t);return e.type==="select"||e.type==="checkbox"||e.type==="textarea"||!e.prefix&&!e.suffix&&!e.clearable?n:R("span",{"data-ui":"field-input","data-disabled":e.disabled?"true":null,"data-invalid":e.error?"true":null,children:[e.prefix&&r("span",{"data-ui":"field-affix",children:e.prefix}),n,e.suffix&&r("span",{"data-ui":"field-affix",children:e.suffix}),e.clearable&&r("button",{"data-ui":"field-clear",type:"button","aria-controls":e.id,"aria-label":e.clearLabel??`Clear ${e.label}`,disabled:e.disabled===!0||e.readOnly===!0,children:r(at,{name:"x",size:"small"})})]})};var co=e=>{if(!/^\d{4}-\d{2}-\d{2}$/.test(e))return null;let[t,n,o]=e.split("-").map(Number);if(t<1||t>9999)return null;let i=new Date(0);return i.setUTCFullYear(t,n-1,o),i.setUTCHours(0,0,0,0),i.getUTCFullYear()===t&&i.getUTCMonth()===n-1&&i.getUTCDate()===o?i:null};var jr=e=>e==="en"?["month","day","year"]:e==="ja"?["year","month","day"]:["day","month","year"],Br=e=>{let t=jr(e).map(n=>({day:"dd",month:"mm",year:"yyyy"})[n]).join("/");return e==="vi"||!e?t:t.toUpperCase()},qr=(e,t,n=!0)=>{if(!co(e))return"";let o={year:e.slice(0,4),month:e.slice(5,7),day:e.slice(8,10)};return jr(t).filter(i=>n||i!=="year").map(i=>o[i]).join("/")};var Pt=e=>({...e,error:e.error??Hr(e.issues,e.name)});var uo=(e,t)=>{let n=e.locale==="en"?{open:t?"Choose date range":"Choose date",title:t?"Date range":"Choose date",previous:"Previous month",next:"Next month",apply:"Apply",cancel:"Cancel",close:"Close calendar",chooseStart:"Choose a start date",chooseEnd:"Choose an end date",chooseDate:"Choose a date",invalidRange:"The end date must be on or after the start date.",presets:"Quick ranges",customRange:"Custom",invalidSelection:"Choose a valid date range.",presetLabels:{today:"Today",yesterday:"Yesterday",last7:"Last 7 days",thisMonth:"This month",lastMonth:"Last month",last30:"Last 30 days",last90:"Last 90 days"}}:e.locale==="ja"?{open:t?"\u671F\u9593\u3092\u9078\u629E":"\u65E5\u4ED8\u3092\u9078\u629E",title:t?"\u671F\u9593":"\u65E5\u4ED8\u3092\u9078\u629E",previous:"\u524D\u306E\u6708",next:"\u6B21\u306E\u6708",apply:"\u9069\u7528",cancel:"\u30AD\u30E3\u30F3\u30BB\u30EB",close:"\u30AB\u30EC\u30F3\u30C0\u30FC\u3092\u9589\u3058\u308B",chooseStart:"\u958B\u59CB\u65E5\u3092\u9078\u629E",chooseEnd:"\u7D42\u4E86\u65E5\u3092\u9078\u629E",chooseDate:"\u65E5\u4ED8\u3092\u9078\u629E",invalidRange:"\u7D42\u4E86\u65E5\u306F\u958B\u59CB\u65E5\u4EE5\u964D\u306B\u3057\u3066\u304F\u3060\u3055\u3044\u3002",presets:"\u671F\u9593",customRange:"\u30AB\u30B9\u30BF\u30E0",invalidSelection:"\u6709\u52B9\u306A\u671F\u9593\u3092\u9078\u629E\u3057\u3066\u304F\u3060\u3055\u3044\u3002",presetLabels:{today:"\u4ECA\u65E5",yesterday:"\u6628\u65E5",last7:"\u904E\u53BB7\u65E5\u9593",thisMonth:"\u4ECA\u6708",lastMonth:"\u5148\u6708",last30:"\u904E\u53BB30\u65E5\u9593",last90:"\u904E\u53BB90\u65E5\u9593"}}:{open:t?"Ch\u1ECDn kho\u1EA3ng ng\xE0y":"Ch\u1ECDn ng\xE0y",title:t?"Kho\u1EA3ng ng\xE0y":"Ch\u1ECDn ng\xE0y",previous:"Th\xE1ng tr\u01B0\u1EDBc",next:"Th\xE1ng sau",apply:"\xC1p d\u1EE5ng",cancel:"H\u1EE7y",close:"\u0110\xF3ng l\u1ECBch",chooseStart:"Ch\u1ECDn ng\xE0y b\u1EAFt \u0111\u1EA7u",chooseEnd:"Ch\u1ECDn ng\xE0y k\u1EBFt th\xFAc",chooseDate:"Ch\u1ECDn m\u1ED9t ng\xE0y",invalidRange:"Ng\xE0y k\u1EBFt th\xFAc ph\u1EA3i b\u1EB1ng ho\u1EB7c sau ng\xE0y b\u1EAFt \u0111\u1EA7u.",presets:"Kho\u1EA3ng th\u1EDDi gian",customRange:"T\xF9y ch\u1EC9nh",invalidSelection:"Vui l\xF2ng ch\u1ECDn kho\u1EA3ng ng\xE0y h\u1EE3p l\u1EC7.",presetLabels:{today:"H\xF4m nay",yesterday:"H\xF4m qua",last7:"7 ng\xE0y qua",thisMonth:"Th\xE1ng n\xE0y",lastMonth:"Th\xE1ng tr\u01B0\u1EDBc",last30:"30 ng\xE0y qua",last90:"90 ng\xE0y qua"}};return{...n,...e.calendarLabels,presetLabels:{...n.presetLabels,...e.calendarLabels?.presetLabels}}},fo=()=>R("svg",{viewBox:"0 0 24 24",width:"16",height:"16",fill:"none",stroke:"currentColor","stroke-width":"1.5","aria-hidden":"true",focusable:"false",children:[r("rect",{x:"3",y:"5",width:"18",height:"16",rx:"2"}),r("path",{d:"M16 3v4M8 3v4M3 11h18"})]}),mo=e=>R("div",{"data-ui":"date-calendar",id:`${e.id}-calendar`,popover:"auto",role:"dialog","aria-labelledby":`${e.id}-calendar-title`,children:[R("header",{"data-ui":"date-calendar-head",children:[r("h3",{"data-ui":"date-calendar-title",id:`${e.id}-calendar-title`,children:e.labels.title}),(e.presets??[]).length>0&&r("div",{"data-ui":"date-range-preset",children:r(Wt,{id:`${e.id}-preset`,name:"",label:e.labels.presets,labelHidden:!0,type:"select",disabled:!0,value:"custom",options:[{value:"custom",label:e.labels.customRange},...(e.presets??[]).map(t=>({value:t,label:e.labels.presetLabels[t]}))]})}),r(ca,{label:e.labels.title,actions:[r(It,{label:e.labels.previous,icon:"\u2039",name:"kv-date-command",value:"previous",size:"compact"}),r(It,{label:e.labels.next,icon:"\u203A",name:"kv-date-command",value:"next",size:"compact"})]}),e.applyOnClose&&r(It,{label:e.labels.close,icon:"\xD7",name:"kv-date-command",value:"close",size:"compact"})]}),r("div",{"data-ui":"date-calendar-body",children:r("div",{"data-ui":"date-calendar-months"})}),R("footer",{"data-ui":"date-calendar-footer",children:[r("p",{"data-ui":"date-calendar-status",role:"status","aria-live":"polite","aria-atomic":"true"}),!e.applyOnClose&&r(ca,{actions:[r(Gt,{label:e.labels.cancel,name:"kv-date-command",value:"cancel"}),r(Gt,{label:e.labels.apply,name:"kv-date-command",value:"apply",variant:"primary"})]})]})]}),xa=e=>{let t=uo(e,!1);return R("div",{"data-ui":"date-picker","data-date-mode":"single","data-date-value":String(e.value??""),"data-date-apply-on-close":e.applyOnClose===!1?null:"true","data-span":e.span??"half","data-date-locale":e.locale??"vi","data-date-today":e.today,"data-date-week-start":e.weekStartsOn??1,"data-date-labels":JSON.stringify(t),children:[r("div",{"data-ui":"date-inputs",children:r(Kt,{...Pt(e),kind:"date",control:r("div",{"data-ui":"date-field-control",children:R("div",{"data-ui":"date-control-row",children:[Ut({...Pt(e),type:"date",appearance:"embedded"},rt(e.id,e.help,Pt(e).error)),r("span",{"data-ui":"date-entry",hidden:!0,children:Ut({...Pt(e),id:`${e.id}-entry`,name:"",type:"text",appearance:"embedded",value:qr(String(e.value??""),e.locale),placeholder:Br(e.locale),inputMode:"numeric",min:void 0,max:void 0,step:void 0},rt(e.id,e.help,Pt(e).error))}),r("div",{"data-ui":"date-tools",children:r(It,{label:`${t.open}: ${e.label}`,icon:r(fo,{}),name:"kv-date-open",controls:`${e.id}-calendar`,expanded:!1,disabled:e.disabled||e.readOnly})})]})})})}),r(mo,{id:e.id,labels:t,applyOnClose:e.applyOnClose!==!1})]})};var zr=e=>{let t=Pt(e),[n="",o=""]=String(e.value??"").split("T"),i={...t,span:"half",help:null,error:null,labelHidden:!0,disabled:!0};return r("div",{"data-ui":"date-time-picker","data-date-mode":"datetime","data-date-locale":e.locale??"vi","data-span":e.span??"half",children:r(Kt,{...t,kind:"datetime-local",control:R("div",{"data-ui":"date-time-controls",children:[Ut({...t,type:"datetime-local"},rt(e.id,e.help,t.error)),R("div",{"data-ui":"date-time-parts",hidden:!0,children:[r(xa,{...i,id:`${e.id}-date`,name:"",value:n,min:e.min?String(e.min).split("T")[0]:void 0,max:e.max?String(e.max).split("T")[0]:void 0,step:"1",locale:e.locale,today:e.today}),r(ho,{...i,id:`${e.id}-time`,name:"",value:o,label:e.locale==="en"?`${e.label}: time`:`${e.label}: gi\u1EDD`,min:void 0,max:void 0})]})]})})})},ho=e=>r(Wt,{...Pt(e),type:"time"});var _r=e=>R("details",{"data-ui":"disclosure",open:e.open===!0?!0:void 0,children:[r("summary",{"data-ui":"disclosure-summary",children:e.meta===void 0&&e.leading===void 0?e.summary:R(P,{children:[e.leading!==void 0&&r("span",{"data-ui":"disclosure-leading",children:e.leading}),r("span",{"data-ui":"disclosure-label",children:e.summary}),e.meta!==void 0&&r("span",{"data-ui":"disclosure-meta",children:e.meta})]})}),r("div",{"data-ui":"disclosure-body",children:e.body})]});var Jr=e=>{let t=e.type==="radio";return r("div",{"data-ui":"field-options","data-orientation":e.optionsOrientation==="vertical"?"vertical":null,role:t?"radiogroup":"group","aria-labelledby":`${e.id}-label`,"aria-describedby":rt(e.id,e.help,e.error),"aria-invalid":e.error?"true":null,children:we(e.options??[],n=>n.name??n.value,n=>R("label",{"data-ui":"field-option",children:[r("input",{"data-ui":"field-option-input",type:t?"radio":"checkbox",name:t?e.name:n.name??`${e.name}[]`,value:n.value,checked:t?String(e.value??"")===n.value:n.checked===!0,required:t&&e.required===!0,disabled:e.disabled===!0||n.disabled===!0,"aria-invalid":e.error?"true":null,"aria-describedby":rt(e.id,e.help,e.error),autocomplete:"off"}),r(ht,{children:n.label})]}))})};var Xr=e=>!!e.error||(e.fields?.some(Xr)??!1),Wt=e=>{if(e.fields)return R("div",{"data-ui":"field","data-kind":"group","data-span":"full","data-invalid":String(!!e.error),children:[r(_r,{summary:e.label,open:e.open||Xr(e),body:r("div",{"data-ui":"form-grid",children:we(e.fields.map(n=>({...n,disabled:e.disabled||n.disabled,readOnly:e.readOnly||n.readOnly})),n=>n.id,n=>r(Wt,{...n}))})}),r(ka,{...e})]});if(e.type==="switch"&&e.control===void 0)return go(e);if(e.type==="datetime-local"&&e.control===void 0)return zr(e);if(e.type==="date"&&e.control===void 0)return xa(e);let t=e.type==="radio"||e.type==="checkbox-group";return r(Kt,{...e,kind:e.type??"text",group:t,control:t?r(Jr,{...e}):Ut(e,rt(e.id,e.help,e.error))})},go=e=>{let t=e.checked===void 0?e.value==="1":e.checked===!0;return r(Kt,{id:e.id,label:e.label,help:e.help,error:e.error,required:e.required,labelHidden:e.labelHidden,span:e.span,size:e.size,kind:"switch",layout:e.layout,control:R("label",{"data-ui":"switch-control",children:[r("input",{id:e.id,type:"checkbox",role:"switch",name:e.name,value:e.value===void 0||e.value===null?"1":String(e.value),checked:t,"aria-checked":String(t),disabled:e.disabled===!0,required:e.required===!0,"aria-invalid":e.error?"true":null,"aria-describedby":rt(e.id,e.help,e.error)}),r("span",{"data-ui":"switch-track","aria-hidden":"true"})]})})};var pt=e=>Array.isArray(e)?e:[],Z=e=>e==null?"":String(e),On=async(e,t,n,o)=>{let i=await fetch(`/_ket/fn/${encodeURIComponent(e)}${o?`?${o.replace(/^\?/,"")}`:""}`,{method:"POST",credentials:"same-origin",headers:{"content-type":"application/json"},body:JSON.stringify(t),signal:n}),s=await i.json();if(!i.ok||s.ok===!1){let m=pt(s.errors)[0];throw new Error(Z(m?.message??m?.code??s.message??`HTTP ${i.status}`))}return s.value},un=new Map,Ur=(e,t)=>{let n=un.get(e)??[];un.set(e,[...n.filter(o=>Z(o.value)!==Z(t.value)),t])},po=(e,t)=>{let n=un.get(e);n&&un.set(e,n.filter(o=>Z(o.value)!==t))};function Wr(e,t={}){let{id:n,config:o}=e,i=o.labels,s=o.manager,m=o.multiple===!0,c=m&&o.presentation==="tags",y=o.triggerId||`${n}-relation-input`,b=Le(m?pt(o.values).map(Z).filter(Boolean):[Z(o.value)].filter(Boolean)),w=()=>b()[0]??"",E=u=>b().includes(Z(u)),k=s?.listFunction?`${s.listFunction}${s.listInput?JSON.stringify(s.listInput):""}`:"",M=pt(o.options),C=un.get(k)??[],K=u=>C.find(L=>Z(L.value)===Z(u)),f=Le([...M.map(u=>K(u.value)??u),...C.filter(u=>!M.some(L=>Z(L.value)===Z(u.value)))]),V=s?.resolveParam?b().filter(u=>!f().some(L=>L.value===u)):[];V.length&&f.set([...f(),...V.map(u=>({value:u,label:o.labels.loading}))]);let A=()=>t.options?.()??f(),j=Le(!Object.keys(s?.listInputFields??{}).length),G=()=>({...s?.listInput}),Y=()=>!j()||(t.disabled?.()??o.disabled===!0),D=Le(!1),ne=Le(!1),Q=Le(""),ke=Le([]),xe=Le(null),me=Le(!1),F=Le(""),q=Le(null),ae=Le(""),te=Le(0),oe=Le(!1),Pe=null,Se=null,Re=null,_e,$e=!1,Ae=u=>Z(u[s?.idField??"id"]),Ne=u=>Z(u[s?.labelField??"name"])||i.noRecords,it=u=>Z(u[s?.descriptionField??"ref"]),Ye=()=>f().find(u=>Z(u.value)===w())?.label??i.choose,bt=()=>{if(s?.searchInDropdown&&xe()?.query===Q().trim())return xe().options.slice(0,7);if(s?.searchInDropdown&&Q().trim())return[];let u=Ke(Q()),L=u?A().filter(U=>Ke(`${Z(U.label)} ${Z(U.description)}`).includes(u)):A();return t.options||!s?L:L.slice(0,7)},Qe=(u,L,U="")=>{if(Y())return;let le=Z(u);if(!f().some(Me=>Z(Me.value)===le)){let Me={value:le,label:Z(L),description:Z(U)};f.set([...f(),Me]),k&&le&&Ur(k,Me)}if(m){b.set(E(le)?b().filter(Me=>Me!==le):[...b(),le]),c&&(D.set(!1),Q.set(""),te.set(0)),t.onSelect?.({value:le,label:Z(L),description:Z(U)});return}b.set(le?[le]:[]),D.set(!1),ne.set(!1),Q.set(""),q.set(null),ae.set(""),t.resetAfterSelect&&b.set([]),t.onSelect?.({value:le,label:Z(L),description:Z(U)})},ot=u=>{b.set(b().filter(L=>L!==Z(u)))},Je=u=>f().find(L=>Z(L.value)===Z(u))?.label??Z(u),Fe=async()=>{if(!s?.listFunction||!j())return;Se?.abort();let u=new AbortController;Se=u,me.set(!0),F.set("");try{let L=G();L[s.searchParam??"search"]=Q().trim(),L[s.limitParam??"limit"]=s.limit??80;let U=await On(s.listFunction,L,u.signal,s.requestQuery);if($e||Se!==u)return;let le=new Set(pt(s.excludeIds).map(Z)),Me=pt(U).filter(W=>!le.has(Ae(W)));ke.set(Me),s.searchInDropdown&&xe.set({query:String(L[s.searchParam??"search"]),options:Me.map(W=>({value:Ae(W),label:Ne(W),description:it(W)}))})}catch(L){if($e||Se!==u||L instanceof Error&&L.name==="AbortError")return;F.set(L instanceof Error?L.message:i.loadError)}finally{Se===u&&me.set(!1)}},xt=async()=>{D.set(!1),ne.set(!0),q.set(null),ae.set(""),await Fe()},je=u=>{u?.stopPropagation();let L=oe();ne.set(!1),q.set(null),ae.set(""),F.set(""),oe.set(!1),L&&lt()},lt=()=>{if(typeof document>"u")return;let u=()=>document.getElementById(y);queueMicrotask(()=>u()?.focus()),typeof requestAnimationFrame=="function"&&requestAnimationFrame(()=>u()?.focus())},st=()=>!!(s?.saveFunction&&s.fields?.length)||!!t.onCreate,$t=()=>{if(Y())return;let u=Q().trim();if(D.set(!1),Q.set(""),s?.saveFunction&&s.fields?.length){oe.set(!0),ne.set(!0),ae.set(""),F.set(""),q.set({id:"",row:{...s.saveDefaults??{},[s.labelField??"name"]:u}}),queueMicrotask(()=>Pe?.querySelector('[data-ui="relation-editor"] [data-ui="form-control"]')?.focus());return}t.onCreate?.(u)},Be=u=>{u.currentTarget instanceof HTMLInputElement&&(Q.set(u.currentTarget.value),Se?.abort(),Se=null,me.set(!0),F.set(""),clearTimeout(_e),_e=setTimeout(Fe,180))},Nt=async u=>{if(u.preventDefault(),!s?.saveFunction||!(u.currentTarget instanceof HTMLFormElement))return;let L=q(),U=L?.id||crypto.randomUUID(),le={...s.saveDefaults??{},id:U};for(let[W,Te]of new FormData(u.currentTarget).entries())le[W]=Z(Te);me.set(!0),F.set("");let Me=u.currentTarget;try{let W=await On(s.saveFunction,le,void 0,s.requestQuery),Te=Z(le[s.labelField??"name"]);if(!L?.id){let Ft=(W&&typeof W=="object"?Ae(W):"")||U;Me.dispatchEvent(new CustomEvent("ket:relation-created",{bubbles:!0,detail:{name:o.name,option:{value:Ft,label:Te}}})),Qe(Ft,Te),oe()?je():m&&(q.set(null),await Fe());return}f.set(f().map(He=>Z(He.value)===U?{...He,label:Te||He.label}:He));let vt=f().find(He=>Z(He.value)===U);k&&vt&&Te&&Ur(k,vt),q.set(null),await Fe()}catch(W){F.set(W instanceof Error?W.message:i.loadError)}finally{me.set(!1)}},Rt=async u=>{let L=Ae(u);if(ae()!==L){ae.set(L);return}if(s?.removeFunction){me.set(!0),F.set("");try{await On(s.removeFunction,{...s.removeDefaults??{},id:L},void 0,s.requestQuery),ot(L),f.set(f().filter(U=>Z(U.value)!==L)),po(k,L),ae.set(""),await Fe()}catch(U){F.set(U instanceof Error?U.message:i.loadError)}finally{me.set(!1)}}},S=u=>{let L=q()?.row??s?.saveDefaults??{};return Wt({id:`${n}-relation-editor-${u.name}`,name:u.name,label:u.label,type:u.type??"text",value:Z(L[u.name]),required:u.required===!0,...u.type==="select"?{options:pt(u.options)}:{}})},h=`${n}-relation-editor`,g=()=>R("div",{"data-ui":"action-group",children:[r("button",{"data-ui":"action","data-variant":"tertiary",type:"button",onClick:()=>oe()?je():q.set(null),children:i.cancel}),r("button",{"data-ui":"action","data-variant":"primary",type:"submit",form:h,disabled:me(),children:i.save})]}),v=()=>R("form",{"data-ui":"relation-editor","data-presentation":oe()?"plain":void 0,id:h,onSubmit:Nt,children:[r("div",{"data-ui":"form-grid","data-columns":"1",children:we(pt(s?.fields),u=>u.name,S)}),oe()?null:r("div",{"data-ui":"form-actions",children:g()})]}),T=u=>{let L=Ae(u),U=it(u);return R("li",{"data-ui":"relation-dialog-row","data-selected":String(E(L)),children:[R("button",{"data-ui":"relation-dialog-main",type:"button","aria-pressed":m?String(E(L)):null,onClick:()=>Qe(L,Ne(u),U),children:[r("strong",{children:Ne(u)}),U?r("small",{"data-ui":"relation-dialog-meta",children:U}):null]}),R("div",{"data-ui":"relation-dialog-actions",children:[r("button",{"data-ui":"action","data-size":"compact","data-variant":E(L)?"primary":"secondary",type:"button",onClick:()=>Qe(L,Ne(u),U),children:E(L)?i.chosen:i.select}),s?.saveFunction?r("button",{"data-ui":"action","data-size":"compact","data-variant":"tertiary",type:"button",onClick:()=>q.set({id:L,row:u}),children:i.edit}):null,s?.removeFunction?r("button",{"data-ui":"action","data-size":"compact","data-variant":"destructive",type:"button",onClick:()=>Rt(u),children:ae()===L?i.confirmRemove:i.remove}):null]})]})},H=()=>R("div",{"data-ui":"modal-layer","data-presentation":"dialog",children:[r("button",{"data-ui":"modal-backdrop",type:"button","aria-label":i.close,onClick:je,children:r("span",{children:i.close})}),R("section",{"data-ui":"modal-sheet","data-size":oe()?"small":void 0,role:"dialog","aria-modal":"true","aria-labelledby":`${n}-relation-title`,children:[R("header",{"data-ui":"modal-head",children:[r("h2",{"data-ui":"modal-title",id:`${n}-relation-title`,children:oe()?i.createTitle??i.create:i.dialogTitle}),r("button",{"data-ui":"modal-close",type:"button","aria-label":i.close,title:i.close,onClick:je,children:"\xD7"})]}),R("div",{"data-ui":"modal-body",children:[oe()?null:R("div",{"data-ui":"relation-dialog-toolbar",children:[R("div",{"data-ui":"relation-search-field",children:[r("span",{"data-ui":"relation-search-icon","aria-hidden":"true",children:R("svg",{viewBox:"0 0 24 24",fill:"none",stroke:"currentColor","stroke-width":"2","stroke-linecap":"round","stroke-linejoin":"round","aria-hidden":"true",focusable:"false",children:[r("circle",{cx:"11",cy:"11",r:"8"}),r("path",{d:"m21 21-4.3-4.3"})]})}),r("input",{"data-ui":"field-control",type:"search",value:Q(),autocomplete:"off",placeholder:i.search,"aria-label":i.search,onInput:Be})]}),s?.saveFunction?r("button",{"data-ui":"action","data-variant":"primary",type:"button",onClick:()=>q.set({id:"",row:s.saveDefaults??{}}),children:i.create}):null]}),F()?R("aside",{"data-ui":"notice","data-tone":"danger",role:"alert",children:[R("div",{"data-ui":"notice-copy",children:[r("p",{"data-ui":"notice-title",children:i.loadError}),r("p",{"data-ui":"notice-message",children:F()})]}),r("button",{"data-ui":"action","data-variant":"secondary",type:"button",onClick:Fe,children:i.retry})]}):null,q()?v():null,me()?r("p",{"data-ui":"relation-empty",role:"status",children:i.loading}):null,!me()&&!q()?ke().length?r("ul",{"data-ui":"relation-dialog-list",children:we(ke(),Ae,T)}):r("p",{"data-ui":"relation-empty",children:i.noRecords}):null]}),oe()&&q()?r("footer",{"data-ui":"modal-actions",children:g()}):null]})]}),$=u=>{if(!(u instanceof KeyboardEvent)||u.key!=="Escape"||!D()&&!ne())return;u.preventDefault(),u.stopPropagation();let L=u.currentTarget?.querySelector('[data-ui="relation-trigger"], [data-ui="relation-input"]');D.set(!1),je(),L?.focus()},B=u=>{u.preventDefault(),D.set(!0),u.currentTarget instanceof HTMLSelectElement&&u.currentTarget.nextElementSibling?.focus()},J=()=>bt().filter(u=>!E(u.value)),N=u=>`${n}-relation-option-${u}`,_=()=>{Y()||D()||(te.set(0),D.set(!0),s?.searchInDropdown&&Fe())},yt=u=>{u.currentTarget instanceof HTMLInputElement&&(s?.searchInDropdown?Be(u):Q.set(u.currentTarget.value),te.set(0),!D()&&!Y()&&D.set(!0))},dt=u=>{if(!(u instanceof KeyboardEvent))return;let L=J();if(u.key==="ArrowDown"||u.key==="ArrowUp"){if(u.preventDefault(),!D()){_();return}let U=u.key==="ArrowDown"?1:-1;te.set(Math.min(Math.max(te()+U,0),Math.max(L.length-1,0)));return}if(u.key==="Enter"){u.preventDefault();let U=D()?L[te()]:void 0;U?Qe(U.value,U.label,U.description):D()&&Q().trim()&&st()?$t():_();return}if(u.key==="Backspace"&&!Q()&&b().length&&!Y()){u.preventDefault(),ot(b().at(-1));return}u.key==="Tab"&&D.set(!1)},ie=()=>R("div",{"data-ui":"relation-select","data-presentation":"tags",role:"group","aria-label":o.ariaLabel,onKeydown:$,children:[R("select",{"data-ui":"relation-native",name:o.name,required:o.required===!0,disabled:Y(),tabindex:"-1","aria-hidden":"true",onInvalid:B,children:[r("option",{value:"",selected:!b().length}),b().length?r("option",{value:b().join(","),selected:!0,children:b().map(Je).join(", ")}):null]}),R("div",{"data-ui":"relation-control","data-disabled":Y()?"true":void 0,"data-invalid":o.invalid===!0?"true":void 0,"data-open":String(D()),onClick:u=>{if(Y())return;let L=u.target;if(L?.closest?.('[data-ui="relation-chip-clear"], [data-ui="relation-clear-all"]'))return;u.currentTarget?.querySelector('[data-ui="relation-input"]')?.focus(),D()&&L?.getAttribute?.("data-ui")!=="relation-input"?D.set(!1):_()},children:[b().length?r("ul",{"data-ui":"relation-chips","aria-label":i.chosen,children:we(b(),u=>u,u=>R("li",{"data-ui":"relation-chip",children:[r("span",{children:Je(u)}),r("button",{"data-ui":"relation-chip-clear",type:"button",tabindex:"-1","aria-label":`${i.clear}: ${Je(u)}`,title:i.clear,disabled:Y(),onClick:()=>ot(u),children:"\xD7"})]}))}):null,r("input",{"data-ui":"relation-input",id:y,type:"text",role:"combobox",value:Q(),autocomplete:"off",placeholder:b().length?"":i.choose,"aria-label":o.ariaLabel,"aria-autocomplete":"list","aria-haspopup":"listbox","aria-expanded":String(D()),"aria-controls":`${n}-relation-options`,"aria-activedescendant":D()&&J().length?N(te()):void 0,"aria-invalid":o.invalid===!0?"true":void 0,"aria-describedby":o.describedBy||void 0,disabled:Y(),onInput:yt,onKeydown:dt}),b().length&&!Y()?r("button",{"data-ui":"relation-clear-all",type:"button",tabindex:"-1","aria-label":i.clear,title:i.clear,onClick:()=>{b.set([]),lt()},children:"\xD7"}):null,r("span",{"data-ui":"relation-caret","aria-hidden":"true",children:"\u25BE"})]}),D()?R("div",{"data-ui":"relation-menu",id:`${n}-relation-menu`,children:[r("div",{"data-ui":"relation-options",id:`${n}-relation-options`,role:"listbox","aria-label":o.ariaLabel,"aria-multiselectable":"true","aria-busy":me()?"true":void 0,children:s?.searchInDropdown&&me()?r("p",{"data-ui":"relation-empty",role:"status",children:i.loading}):s?.searchInDropdown&&F()?r("p",{"data-ui":"relation-empty",role:"alert",children:F()}):J().length?we(J().map((u,L)=>({entry:u,index:L})),({entry:u})=>u.value,({entry:u,index:L})=>R("button",{"data-ui":"relation-option",id:N(L),type:"button",role:"option",tabindex:"-1","data-active":String(L===te()),"aria-selected":"false",onMousedown:U=>U.preventDefault(),onClick:()=>Qe(u.value,u.label,u.description),children:[r("span",{children:u.label}),u.description?r("small",{children:u.description}):null]})):r("p",{"data-ui":"relation-empty",children:i.noRecords})}),st()||s?R("footer",{"data-ui":"relation-footer",children:[st()?r("button",{"data-ui":"action","data-variant":"tertiary",type:"button",tabindex:"-1",disabled:Y(),onMousedown:u=>u.preventDefault(),onClick:$t,children:Q().trim()&&i.createNamed?i.createNamed.replace("{query}",Q().trim()):i.create}):null,s?.listFunction?r("button",{"data-ui":"action","data-variant":"tertiary",type:"button",tabindex:"-1",onMousedown:u=>u.preventDefault(),onClick:xt,children:i.more}):null]}):null]}):null,ne()?H():null]});return{getSelectedValues:()=>[...b()],view:()=>c?ie():R("div",{"data-ui":"relation-select",role:"group","aria-label":o.ariaLabel,onKeydown:$,children:[R("select",{"data-ui":"relation-native",name:o.name,required:o.required===!0,disabled:Y(),tabindex:"-1","aria-hidden":"true",onInvalid:B,children:[r("option",{value:"",selected:!b().length}),m?b().length?r("option",{value:b().join(","),selected:!0,children:b().map(Je).join(", ")}):null:we(f().filter(u=>Z(u.value)),u=>u.value,u=>r("option",{value:u.value,selected:Z(u.value)===w(),children:u.label}))]}),R("button",{"data-ui":"relation-trigger",id:o.triggerId,"aria-invalid":o.invalid===!0?"true":void 0,"aria-describedby":o.describedBy||void 0,"data-empty":String(!b().length),type:"button","aria-haspopup":"listbox","aria-label":o.ariaLabel,"aria-expanded":String(D()),"aria-controls":`${n}-relation-menu`,disabled:Y(),onClick:u=>{D.set(!D()),D()&&s?.searchInDropdown&&Fe();let L=u.currentTarget?.parentElement;D()&&typeof L?.querySelector=="function"&&queueMicrotask(()=>L.querySelector('[data-ui="relation-search"]')?.focus())},children:[r("span",{"data-ui":"relation-value",children:m?i.choose:Ye()}),r("span",{"data-ui":"relation-caret","aria-hidden":"true",children:"\u25BE"})]}),m&&b().length?r("ul",{"data-ui":"relation-chips","aria-label":i.chosen,children:we(b(),u=>u,u=>R("li",{"data-ui":"relation-chip",children:[r("span",{children:Je(u)}),r("button",{"data-ui":"relation-chip-clear",type:"button","aria-label":`${i.clear}: ${Je(u)}`,title:i.clear,disabled:Y(),onClick:()=>ot(u),children:"\xD7"})]}))}):null,D()?R("div",{"data-ui":"relation-menu",id:`${n}-relation-menu`,role:"group","aria-label":o.ariaLabel,children:[r("input",{"data-ui":"relation-search",type:"search",value:Q(),autocomplete:"off",placeholder:i.search,"aria-label":i.search,onInput:u=>{s?.searchInDropdown?Be(u):u.currentTarget instanceof HTMLInputElement&&Q.set(u.currentTarget.value)}}),r("div",{"data-ui":"relation-options",role:"listbox","aria-busy":me()?"true":void 0,children:s?.searchInDropdown&&me()?r("p",{"data-ui":"relation-empty",role:"status",children:i.loading}):s?.searchInDropdown&&F()?R("div",{children:[r("p",{"data-ui":"relation-empty",role:"alert",children:F()}),r("button",{"data-ui":"action","data-variant":"tertiary",type:"button",onClick:()=>{Fe()},children:i.retry})]}):bt().length?we(bt(),u=>u.value,u=>R("button",{"data-ui":"relation-option",type:"button",role:"option","data-selected":String(E(u.value)),"aria-selected":String(E(u.value)),onClick:()=>Qe(u.value,u.label,u.description),children:[r("span",{children:u.label}),u.description?r("small",{children:u.description}):null]})):r("p",{"data-ui":"relation-empty",children:i.noRecords})}),s||t.onCreate?R("footer",{"data-ui":"relation-footer",children:[s?r("button",{"data-ui":"action","data-variant":"tertiary",type:"button",onClick:xt,children:i.more}):null,t.onCreate?r("button",{"data-ui":"action","data-variant":"tertiary",type:"button",disabled:Y(),onClick:()=>{if(Y())return;let u=Q().trim();D.set(!1),Q.set(""),t.onCreate?.(u)},children:i.create}):null]}):null]}):null,ne()?H():null]}),mount:({root:u,lifetime:L})=>{if(Pe=typeof HTMLElement<"u"&&u instanceof HTMLElement?u:null,typeof Element<"u"&&u instanceof Element){let fe=u.closest("form");(fe??u).addEventListener("reset",se=>{let he=se.target;he!==fe&&(!(he instanceof Element)||![...u.querySelectorAll('[data-ui="relation-native"]')].some(pe=>pe instanceof HTMLSelectElement&&pe.name===o.name&&he.contains(pe)))||queueMicrotask(()=>{!$e&&!L.aborted&&!se.defaultPrevented&&b.set(m?pt(o.values).map(Z).filter(Boolean):[Z(o.value)].filter(Boolean))})},{signal:L,capture:!0})}let U=Object.entries(s?.listInputFields??{});if(U.length){let fe=u.closest("form");G=()=>({...s?.listInput,...Object.fromEntries(U.map(([ye,ut])=>{let ft=fe?.elements.namedItem(ut);return[ye,ft&&"value"in ft?Z(ft.value):""]}))});let se=()=>U.map(([ye])=>Z(G()[ye])),he=se();j.set(he.every(Boolean));let pe=()=>{let ye=se();ye.every((ut,ft)=>ut===he[ft])||(he=ye,Se?.abort(),Se=null,Re?.abort(),clearTimeout(_e),me.set(!1),F.set(""),Q.set(""),D.set(!1),ne.set(!1),ke.set([]),xe.set(null),f.set([]),b.set([]),j.set(ye.every(Boolean)))};fe?.addEventListener("change",pe,{signal:L}),fe?.addEventListener("input",pe,{signal:L})}if(V.length&&s?.resolveParam){let fe=new AbortController;Re=fe,L.addEventListener("abort",()=>fe.abort(),{once:!0}),Promise.all(V.map(async se=>{let he=await On(s.listFunction,{...G(),[s.resolveParam]:se,[s.limitParam??"limit"]:1},fe.signal,s.requestQuery);return pt(he).find(pe=>Ae(pe)===se)})).then(se=>{if($e||fe.signal.aborted)return;let he=new Map(se.filter(pe=>!!pe).map(pe=>[Ae(pe),pe]));f.set(f().map(pe=>{if(!V.includes(pe.value))return pe;let ye=he.get(pe.value);return{value:pe.value,label:ye?Ne(ye):i.noRecords,description:ye?it(ye):null}}))}).catch(()=>{!$e&&!fe.signal.aborted&&F.set(i.loadError)})}let le=!0,Me=kt(()=>{if(b(),le){le=!1;return}queueMicrotask(()=>{if($e||L.aborted)return;let fe=[...u.querySelectorAll('[data-ui="relation-native"]')][0],se=c?'[data-ui="relation-input"]':'[data-ui="relation-trigger"]',he=u instanceof HTMLElement?u.querySelector(se):null,pe=u instanceof HTMLElement?u.parentElement:null;if(fe instanceof HTMLSelectElement&&fe.dispatchEvent(new Event("change",{bubbles:!0})),(!m||c)&&!D()&&!ne()&&!Y()){let ye=()=>{let ut=he?.isConnected?he:pe?.isConnected&&he?.id?[...pe.querySelectorAll(se)].find(ft=>ft.id===he.id):null;ut&&!ut.hasAttribute("disabled")&&Fr(ut,document.activeElement,document.body,W)};ye(),he&&!he.isConnected&&requestAnimationFrame(ye)}})});L.addEventListener("abort",Me);let W=null,Te=0,vt=()=>{if($e||L.aborted||!D()||!W?.isConnected)return;let fe=[...u.querySelectorAll('[data-ui="relation-trigger"], [data-ui="relation-control"]')][0];if(!(fe instanceof HTMLElement))return;let se=getComputedStyle(W),he=window.visualViewport;W.style.width=`${fe.getBoundingClientRect().width}px`,W.style.maxHeight="";let pe=W.getBoundingClientRect(),ye=Nr({anchor:fe.getBoundingClientRect(),width:pe.width,height:pe.height,viewport:{left:he?.offsetLeft??0,top:he?.offsetTop??0,width:he?.width??window.innerWidth,height:he?.height??window.innerHeight},gap:parseFloat(se.scrollMarginTop),inset:parseFloat(se.scrollMarginInlineStart),rtl:getComputedStyle(fe).direction==="rtl"});W.style.left=`${ye.left}px`,W.style.top=`${ye.top}px`,W.style.width=`${ye.width}px`,W.style.maxHeight=`${ye.maxHeight}px`},He=()=>{cancelAnimationFrame(Te),Te=requestAnimationFrame(vt)},Ft=kt(()=>{let fe=D();bt(),me(),F(),queueMicrotask(()=>{if($e||L.aborted)return;if(!fe){W?.isConnected&&W.matches(":popover-open")&&W.hidePopover(),W=null;return}let se=[...u.querySelectorAll('[data-ui="relation-menu"]')][0];if(!(se instanceof HTMLElement)||typeof se.showPopover!="function")return;let he=W!==se;W=se,se.setAttribute("popover","manual"),se.matches(":popover-open")||se.showPopover(),vt(),he&&se.querySelector('[data-ui="relation-search"]')?.focus()})}),fn=()=>{Ft(),cancelAnimationFrame(Te),W?.isConnected&&W.matches(":popover-open")&&W.hidePopover(),W=null};L.addEventListener("abort",fn,{once:!0}),window.addEventListener("resize",He,{signal:L}),window.addEventListener("scroll",He,{capture:!0,signal:L}),window.visualViewport?.addEventListener("resize",He,{signal:L}),window.visualViewport?.addEventListener("scroll",He,{signal:L}),document.addEventListener("focusout",fe=>{if(!D()||!(u instanceof Node)||!(fe.target instanceof Node)||!u.contains(fe.target))return;let se=fe.relatedTarget;se instanceof Node&&!u.contains(se)&&D.set(!1)},{signal:L}),document.addEventListener("click",fe=>{if(!D())return;let se=fe.target;se instanceof Node&&u.contains(se)||D.set(!1)},{signal:L})},dispose:()=>{$e=!0,clearTimeout(_e),Se?.abort(),Re?.abort()}}}var bo={"design-system.relation-select":e=>Wr(e),"design-system.ket-grid":$r},Zr=Qn(Hn(),bo,{strict:!1}),Ra=()=>{Zr.hydrate(document)};document.readyState==="loading"?document.addEventListener("DOMContentLoaded",Ra,{once:!0}):Ra();document.addEventListener("ket:navigation-start",()=>{for(let e of document.querySelectorAll('[data-ket-slot="demo.content"], [data-ket-slot="demo.overlay"]'))Zr.dispose(e)});document.addEventListener("ket:navigation-complete",Ra);})();
