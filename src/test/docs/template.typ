#let theme = {
  set page(
    paper: "a4",
    margin: (top: 17mm, bottom: 18mm, x: 18mm),
    numbering: "1",
  )
  set text(
    font: ("PingFang SC", "Helvetica Neue", "Arial Unicode MS", "Menlo"),
    size: 10.5pt,
  )
  set par(justify: false, leading: 0.45em)
  set heading(numbering: "1.")
  set table(stroke: 0.6pt + luma(185))
  show link: set text(fill: rgb("#1E5BB8"))

  show heading.where(level: 1): it => block(
    inset: (top: 8pt, bottom: 6pt),
    fill: rgb("#F4F8FF"),
    stroke: (left: 4pt + rgb("#2F6FED")),
    radius: 4pt,
  )[
    #text(14pt, weight: "bold", fill: rgb("#173B7A"))[#it.body]
  ]

  show heading.where(level: 2): it => [
    #v(6pt)
    #text(12pt, weight: "bold", fill: rgb("#214A91"))[#it.body]
    #v(3pt)
  ]
}

#let title-block(title, subtitle: none, version: [V1.0], date: auto) = block(
  fill: rgb("#EEF4FF"),
  inset: 14pt,
  radius: 8pt,
  stroke: 0.8pt + rgb("#B9CCF5"),
)[
  #align(center)[
    #text(20pt, weight: "bold", fill: rgb("#0F2D63"))[#title]
    #if subtitle != none [
      #v(4pt)
      #text(11pt, fill: rgb("#3A578A"))[#subtitle]
    ]
    #v(8pt)
    #text(9pt, fill: rgb("#586D93"))[
      版本：#version  \
      日期：#date
    ]
  ]
]

#let info-box(title, body) = block(
  fill: rgb("#FFF7E9"),
  inset: 10pt,
  radius: 6pt,
  stroke: 0.8pt + rgb("#F0C36B"),
)[
  #text(10.5pt, weight: "bold", fill: rgb("#8A5A00"))[#title]
  #v(4pt)
  #body
]

#let note-box(title, body, warning: false) = {
  let fill-color = if warning { rgb("#FFF1EE") } else { rgb("#EEF7FF") }
  let stroke-color = if warning { rgb("#D85B43") } else { rgb("#4E87C7") }
  let title-color = if warning { rgb("#8F2F22") } else { rgb("#214F82") }
  block(
    width: 100%,
    fill: fill-color,
    inset: 10pt,
    radius: 6pt,
    stroke: (left: 3pt + stroke-color),
  )[
    #text(10.5pt, weight: "bold", fill: title-color)[#title]
    #v(4pt)
    #body
  ]
}

#let command(code) = block(
  width: 100%,
  fill: rgb("#17202B"),
  inset: 9pt,
  radius: 5pt,
)[
  #set text(font: "Menlo", size: 8.5pt, fill: rgb("#E8EEF5"))
  #raw(code, block: true, lang: "bash")
]

#let screenshot(path, caption) = figure(
  block(
    width: 100%,
    inset: 3pt,
    radius: 5pt,
    stroke: 0.7pt + luma(185),
    image(path, width: 100%),
  ),
  caption: caption,
)
