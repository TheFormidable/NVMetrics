# NVMetrics 0.14.7 Candidate — Intel

Instalador autónomo para macOS x86_64 (mínimo declarado: macOS 14), una RTX 5050 y NullMoth compatible ya instalado. Incluye el runtime Python; el usuario no necesita Homebrew, Python ni Xcode.

[Instalador histórico (commit archivado)](https://github.com/TheFormidable/NVMetrics/blob/99e2399493123791ca5482b18907104c7d037d4f/packages/0.14.7/NVMetrics-0.14.7-Candidate-Intel.pkg) · [SHA-256](SHA256SUMS.txt) · [Código, compilación y pruebas](../../rm-controller/README.md) · [Auditoría](../../rm-controller/AUDITORIA-v0147.md)

Los instaladores compilados ya no se incluyen en el árbol de código fuente. Las futuras distribuciones deben publicarse como assets de GitHub Releases.

Candidata de estabilización. Falta validar físicamente el recorrido completo 100→30 % en esta revisión. App con firma ad hoc; paquete sin firma Developer ID ni notarización.

Este instalador es el de la 0.14.7. El código de [`rm-controller/`](../../rm-controller/README.md) ya es la 0.14.8 (Reposo al momento con el driver 3.0.1 y arreglo del menú de la barra), que todavía no tiene instalador publicado: se compila desde ahí y se instala encima de esta.
