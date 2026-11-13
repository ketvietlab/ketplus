# JetBrains importer fixtures

`dark.icls`, `light.icls` and `sample.theme.json` are original synthetic fixtures
written for KetPlus tests. Copyright 2026 KetPlus contributors; licensed under
the repository's MPL-2.0 license. No IDE palette or third-party theme is copied.
The test creates a small JAR in memory from these fixtures plus an inert fake
`.class` resource and a plugin descriptor, under the same license. No Java code
is executed and no JAR tooling or IDE installation is required.

Format references (not fixture sources):
- https://plugins.jetbrains.com/docs/intellij/themes-customize.html
- https://plugins.jetbrains.com/docs/intellij/themes-extras.html
- IntelliJ serialization reference at commit
  `09f03735d4674eea9f112165fe45744a25fbdd56`:
  https://github.com/JetBrains/intellij-community/blob/09f03735d4674eea9f112165fe45744a25fbdd56/platform/core-api/src/com/intellij/openapi/editor/markup/AttributesFlyweightSerDe.java
