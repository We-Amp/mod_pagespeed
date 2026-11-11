- **Admin console view controls no longer reload the page.** Changing the Logs
  source or "Group repeats", or the Rule packs log selection, keeps the filter
  text, chosen levels, expanded rows, scroll position and keyboard focus, and
  no longer adds a Back step per change.
- **The admin console menu at phone width and high zoom behaves like a drawer.**
  When closed it is out of the tab order; when open, focus moves into it, the
  page behind cannot be reached, Escape closes it and focus returns to the
  menu button.
- **Rule packs no longer shows one pack selected while the table still shows
  another** after a failed read; a note names both until the read succeeds.
- **Every admin console control shows where the keyboard is.** Text fields,
  selects, tabs and menu links draw a complete focus ring, also in Windows
  high-contrast mode, where the current page, selected tab and chart bars now
  stay visible. At 320 px wide (400% zoom) no page scrolls sideways.
- **Admin console screen-reader support.** Filter result counts are announced
  after typing stops, table headers carry their scope and row headers name the
  row, the Savings tables are named, and small controls meet the 24 px target
  size. The accessibility checks now cover WCAG 2.2 AA.
