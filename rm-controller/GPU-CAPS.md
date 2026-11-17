# GPU Caps: estado de la integración (17-11-2026)

## Arquitectura y contrato

| Fase | Componente | Resultado |
|---|---|---|
| 1 | NVRM, selector 14, ABI 1.0 | Capacidades cacheadas por GPU, consulta exclusivamente de lectura |
| 2B | Driver | Publicación de capacidades adicionales observadas por RM |
| 2C | Agente C | `caps-live`: observaciones RUSD independientes de la autorización |
| 3 | Agente y SwiftUI | `gpu_profile` en telemetría, slots dispersos y presentación dinámica |

Los estados `verified`, `unknown` y demás estados definidos por la ABI no son permisos. Una respuesta válida de un tacómetro demuestra que responde, no que esté calibrado. El perfil no presupone que cada tacómetro sea un ventilador físico independiente. No se deducen permisos de un GET exitoso. Un driver anterior sin ABI no gana capacidades por defecto.

`gpu_profile` contiene `gpu_id`, `tach_slot_mask`, `thermal_target_mask` y `power_read_mask`. Los bits de tacómetros se acumulan según observaciones positivas de la sesión; las demás máscaras describen la lectura actual. Los slots son índices, no un recuento de motores. Una RPM faltante o un canal sin datos no prueba ausencia física ni Fan Stop. La interfaz admite slots dispersos hasta 16; agentes anteriores sin perfil muestran únicamente RPM positivas observadas. El perfil no constituye una prueba de frescura para el guard.

## Evidencia real

RTX 5050 ASUS, PCI 10de:2d83, revisión a1, subsistema 1043:8a3f; GB207-A, RM/GSP 620.70.06, VBIOS 98.07.1D.00.C0. Intel macOS 26.7.3, Xcode 26.5 (17F42).

| Prueba | Resultado reportado |
|---|---|
| Fase 1 | ABI usable, 9 verificadas, 156 desconocidas, manual false |
| Fase 2B | ABI usable, 15 verificadas, 150 desconocidas, generación 1 y 2 tras reanudación, manual false |
| Fase 2C | GPU ID 256, tach 0x3, thermal 0x3, power 0x7; cooler_count y fan_stop desconocidos |
| Fase 3 | GPU 56.7 °C, VRAM 80 °C, 48.28 W instantáneos / 48.24 W medios, límite 130 W, 1210/1209 RPM, XID/bugchecks/RM asserts 0 en esa muestra |
| Paquete 0.15.0 | Compilación nativa, instalación y funcionamiento de la GUI confirmados por usuario |

Estos resultados representan una tarjeta y una configuración, no calibración ni certificación general. Las temperaturas y RPM varían con la carga.

## Límites actuales

| Función | Estado |
|---|---|
| Temperaturas GPU/VRAM, potencia, RPM | Lecturas confirmadas en la tarjeta de prueba |
| Número físico de ventiladores e IDs de sus controladores | No determinado universalmente |
| Rangos RPM/PWM físicos | No certificados; valores candidatos del layout RM conocido no son límites universales |
| Fan Stop, límites térmicos de firmware, mapeo cooler/tacómetro | Desconocidos o no validados |
| Escritura manual dinámica por capacidades | No implementada; permanece la cualificación preexistente |
| Otras GPU y multi-GPU | Arquitectura de descubrimiento extensible; no validadas físicamente; agente selecciona GPU por defecto |
| Distribución | Candidato Intel local; sin notarización ni certificación pública |

`manual_authorized:false` en el diagnóstico no deshabilita ni certifica la ruta manual heredada. Esa ruta tiene su propia cualificación, guard y verificación de AUTO. Se conserva el mínimo 30%, entrada a 50%, máximo 100%, reducciones por escalones de 5 puntos porcentuales, vigilancia térmica y de muestras, read-back y rescate independiente. El perfil continuo no implica vigilancia desatendida segura. No se modifica la política de Eco o Reposo.

## Reproducibilidad y próximos pasos

Consulta [construcción](../LEEME-GPU-CAPS.md). `tests/test_gpu_profile.swift` prueba decodificación de índices dispersos y compatibilidad con agentes anteriores; `run_tests.sh` lo incorpora. La validación de firmware adicional, pruebas multi-GPU, calibración física y recuperación bajo fallos preceden cualquier ampliación de permisos. El tamaño del paso manual queda pendiente y fuera del alcance de estas fases.
