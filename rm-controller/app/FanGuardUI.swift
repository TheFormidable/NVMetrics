import SwiftUI
import AppKit

struct FanGuardMenu: View {
    @EnvironmentObject private var fanGuard: FanGuardUIModel
    var body: some View {
        Menu("Ventiladores · Control RM") {
            Text(fanGuard.rmStatus)
            if fanGuard.active {
                Text("Aplicado: \(fanGuard.readbackConfirmed ? "\(fanGuard.currentPercent) %" : "Sin confirmar")")
                Text("Seleccionado: \(fanGuard.targetPercent) %")
                Button("Seleccionar +5 %") { fanGuard.selectPercent(fanGuard.targetPercent + 5) }
                    .disabled(fanGuard.busy || fanGuard.targetPercent >= 100)
                Button("Seleccionar −5 %") { fanGuard.selectPercent(fanGuard.targetPercent - 5) }
                    .disabled(fanGuard.busy || fanGuard.targetPercent <= fanGuard.selectionFloor)
                Menu("Seleccionar velocidad") {
                    ForEach(Array(stride(from: fanGuard.selectionFloor, through: 100, by: 5)), id: \.self) { pct in
                        Button("\(pct) %\(fanGuard.targetPercent == pct ? " ✓" : "")") { fanGuard.selectPercent(pct) }
                    }
                }.disabled(fanGuard.busy)
                Button("Aplicar \(fanGuard.targetPercent) %") { fanGuard.applySelection() }.disabled(!fanGuard.canApply)
                Button("Restaurar Automático") { fanGuard.restoreAuto() }
            } else {
                Text(fanGuard.physicalEnabled ? (fanGuard.oneShotUsed ? "Rescate pendiente o sesión no rearmada" : "Sesión de prueba disponible") : "Servicio físico no habilitado")
                Text("Abre la ventana para iniciar Manual \(fanGuard.manualFloor) %")
            }
            Button("Consultar servicio") { fanGuard.refresh() }
        }
        .onAppear { fanGuard.refresh() }
    }
}

struct FanGuardCard: View {
    @EnvironmentObject private var fanGuard: FanGuardUIModel
    @Environment(\.colorScheme) private var scheme
    @Environment(\.openWindow) private var openWindow
    @State private var confirmStart = false

    var body: some View {
        let th = Theme(scheme: scheme)
        return Card(icon: "fanblades.fill", title: "Control RM · sesión física protegida") {
            Text(fanGuard.rmStatus)
                .font(.system(size: 13, weight: .semibold)).foregroundStyle(th.text)
            Button("Ver comportamiento AUTO del firmware…") { openWindow(id: "firmware-curve"); NSApp.activate(ignoringOtherApps: true) }
            if fanGuard.active {
                HStack(alignment: .center, spacing: 8) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("VELOCIDAD APLICADA").font(.system(size: 10, weight: .semibold)).foregroundStyle(th.label)
                        Text(fanGuard.readbackConfirmed ? "\(fanGuard.currentPercent)%" : "—").font(.system(size: 30, weight: .bold, design: .rounded)).foregroundStyle(th.text).monospacedDigit()
                        Text(fanGuard.readbackConfirmed ? "Confirmada por RM" : "Sin confirmar").font(.system(size: 10)).foregroundStyle(th.label)
                    }.frame(minWidth: 120, alignment: .leading)
                    Divider().frame(height: 52)
                    VStack(alignment: .leading, spacing: 3) {
                        Text("VELOCIDAD SELECCIONADA").font(.system(size: 10, weight: .semibold)).foregroundStyle(th.label)
                        Text("\(fanGuard.targetPercent)%").font(.system(size: 30, weight: .bold, design: .rounded)).foregroundStyle(Theme.green).monospacedDigit()
                        Text("Los ventiladores funcionarán al \(fanGuard.targetPercent) % al aplicar.").font(.system(size: 11)).foregroundStyle(th.text)
                    }
                    Spacer(minLength: 0)
                }
                VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 8) {
                    Button("−5 %") { fanGuard.selectPercent(fanGuard.targetPercent - 5) }
                        .disabled(fanGuard.busy || fanGuard.targetPercent <= fanGuard.selectionFloor)
                    // A discrete slider stages a value only. No hardware writes on drag.
                    if fanGuard.selectionFloor < 100 {
                        Slider(value: Binding(get: { Double(fanGuard.targetPercent) }, set: { fanGuard.selectPercent(Int($0.rounded())) }), in: Double(fanGuard.selectionFloor)...100.0, step: 5)
                            .disabled(fanGuard.busy)
                    }
                    Button("+5 %") { fanGuard.selectPercent(fanGuard.targetPercent + 5) }
                        .disabled(fanGuard.busy || fanGuard.targetPercent >= 100)
                }
                HStack(spacing: 8) {
                    Picker("", selection: Binding(get: { fanGuard.targetPercent }, set: { fanGuard.selectPercent($0) })) {
                        ForEach(Array(stride(from: fanGuard.selectionFloor, through: 100, by: 5)), id: \.self) { pct in
                            Text("\(pct) %").tag(pct)
                        }
                    }.labelsHidden().frame(width: 75).disabled(fanGuard.busy)
                    Button(fanGuard.busy ? "Aplicando…" : "Aplicar \(fanGuard.targetPercent) %") { fanGuard.applySelection() }
                        .buttonStyle(.borderedProminent).tint(Theme.greenDim).disabled(!fanGuard.canApply)
                    Spacer(minLength: 0)
                    Button("AUTO") { fanGuard.restoreAuto() }
                }
                }
                if fanGuard.busy {
                    ProgressView().controlSize(.small)
                } else if fanGuard.targetPercent != fanGuard.currentPercent {
                    Text("Cambio pendiente · todavía no se ha aplicado.").font(.system(size: 11, weight: .medium)).foregroundStyle(Theme.warn)
                } else {
                    Text("Sin cambios pendientes.").font(.system(size: 11)).foregroundStyle(th.label)
                }
            } else {
                VStack(alignment: .leading, spacing: 8) {
                    Button("Activar Manual · 50 %") { confirmStart = true }
                        .buttonStyle(.borderedProminent).tint(Theme.greenDim).disabled(!fanGuard.canBegin)
                    Button("Actualizar estado") { fanGuard.refresh() }.disabled(fanGuard.busy)
                    if fanGuard.busy {
                        ProgressView().controlSize(.small)
                        Button("Automático") { fanGuard.restoreAuto() }
                    }
                }
            }
            Text(fanGuard.continuousProfile ? "Manual continuo 30–100 %. El arranque permanece en 50 %; el programa no baja automáticamente al 30 %. Puedes subir a un valor mayor en una operación, con confirmación RM. Las bajadas requieren escalones verificados. AUTO ante límites térmicos o pérdida de supervisión. Prueba presencial." : fanGuard.thermalProfile ? "Objetivo inicial 30 %. Arranque verificado al 50 %: GPU <60 °C, VRAM <84 °C, ≤60 W. Bajar de 50 %: GPU <58 °C, VRAM <82 °C, ≤50 W y uso ≤20 %. AUTO ante límites térmicos o fallo. No pruebes juegos a baja velocidad todavía." : fanGuard.loadProfile ? "Carga supervisada: Manual 50–100 %. Las reducciones no están habilitadas." : "Sólo inactividad: mínimo 30 %, ambos ventiladores ≥1000 RPM. Nunca uses <50 % jugando.")
                .font(.system(size: 11)).foregroundStyle(th.label).fixedSize(horizontal: false, vertical: true)
            if let text = fanGuard.message {
                Text(text).font(.system(size: 11)).foregroundStyle(th.label)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .confirmationDialog("Ensayo real de ventiladores", isPresented: $confirmStart, titleVisibility: .visible) {
            Button("Activar Manual al 50 %") { fanGuard.begin() }
            Button("Cancelar", role: .cancel) { }
        } message: {
            Text(fanGuard.loadProfile ? "Prueba de carga EXPERIMENTAL: ambos ventiladores deben girar, los sensores deben responder y AUTO se activará por límites térmicos. Comienza en reposo, supervisa en persona y nunca confíes en recuperación durante un fallo kernel/driver." : "Escritura RM privada. Inicia en reposo con servicio root y supervisión. Sin garantía ante un fallo de driver/kernel.")
        }
        .task {
            while !Task.isCancelled {
                fanGuard.refresh()
                do { try await Task.sleep(nanoseconds: 5_000_000_000) }
                catch { break }
            }
        }
    }
}
