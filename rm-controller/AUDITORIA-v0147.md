# Auditoría 0.14.7: demora al reducir ventiladores

La sesión real de 0.14.6 del 16 de noviembre mostró unos 8 s por escalón. El recorrido 100→30 terminó con ambos tacómetros verificados. Una bajada posterior 60→30 se detuvo antes de 45 % por lecturas de 50,03 / 50,86 / 50,10 W, por encima del límite de 50 W. No hubo sobretemperatura en ese rechazo: GPU 46,1 °C y VRAM 68 °C. Estos eventos proceden de los registros locales de la instalación que realizó el usuario.

## Corrección

Se mantiene un proceso lector RUSD de sólo lectura durante Manual. Inicializa el mapeo una vez y responde únicamente a SAMPLE con identificador de petición. No acepta comandos de ventiladores, boost ni Eco. Cada respuesta toma una nueva instantánea y requiere avance de las cuatro fuentes críticas desde la respuesta anterior. Timeout, EOF, identificador incorrecto, datos repetidos o frescura inválida cierran el lector y conservan la ruta de fallo segura existente. El servidor cierra el lector al finalizar Manual y al apagarse.

Se conservan todos los límites, los escalones de 5 %, las tres muestras de RPM separadas por 450 ms, la pausa entre escalones de 700 ms, el guard y el rescate. No se amplía el límite de 50 W. El rechazo ahora explica el valor concreto y conserva el porcentaje seleccionado para un reintento explícito.

## Medición real sin escrituras de hardware

Agente extraído del instalador 0.14.7, RTX 5050 con NullMoth:

- Lector anterior: mediana **1.213 s** por sondeo (3 sondeos).
- Nuevo lector: arranque único de **1.210 s**.
- Sondeos posteriores: mediana **0.180 ms** (12 sondeos, separados por 450 ms).
- Se comprobaron las cuatro fuentes nuevas en cada respuesta. El proceso terminó al cerrar stdin.

La medición corresponde al coste del sondeo, no al tiempo total de un cambio físico. La bajada conserva esperas de seguridad: no será instantánea. El tiempo completo 100→30 de esta revisión debe medirse después de instalarla y realizar una prueba supervisada. No se ejecutó SET, cambio de modo ni instalación durante este trabajo.

## Validación y entrega

101 pruebas Python aprobadas, 10 suites C (incluida la nueva de protocolo/frescura del lector), dos suites Swift y las pruebas shell originales. Las pruebas incluyen errores de lector, cierre, reutilización, respuestas duplicadas, timeout, selección conservada, mensaje con 50,10 W y migración desde 0.14.6. Las pruebas antiguas de procesos simulados utilizan ahora el intérprete del test para evitar depender del arranque del Python de Xcode.

El instalador Intel se compiló y pasó extracción, comparación de payload y verificación profunda de firma de la aplicación. Los 49 Mach-O contienen x86_64 y no enlazan rutas absolutas de bibliotecas externas al sistema. Python viene incluido. App firmada ad hoc; paquete sin firma Developer ID/notarización. Requiere NullMoth instalado.

El código está separado en development-v0147. Se conservan development-v0146, la instalación activa y los entregables anteriores. Se incluye parche contra 0.14.6 y descripción de PR para revisión. La publicación conserva el código previo de la raíz e incorpora esta revisión en `rm-controller/`. La autorización por UID y los requisitos de cualificación pública siguen siendo los descritos en la revisión anterior.
