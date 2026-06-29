#let theme = {
  set page(
    paper: "a4",
    margin: (x: 18mm, y: 16mm),
  )
  set text(
    font: ("PingFang SC", "Helvetica Neue", "Arial Unicode MS", "Menlo"),
    size: 10.5pt,
  )
  set par(justify: false, leading: 0.45em)
  set heading(numbering: "1.")
  set table(stroke: 0.6pt + luma(185))

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
