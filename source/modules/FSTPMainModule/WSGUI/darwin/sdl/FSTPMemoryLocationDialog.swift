import SwiftUI

@available(macOS 13.0, *)
class MemoryLocationViewModel: ObservableObject {
    @Published var locationNumber: String = ""
    @Published var timecode: String = ""
    @Published var name: String = ""
    @Published var comments: String = ""
    @Published var recallZoom: Bool = false

    let playerId: Int
    let currentTime: Double
    let editingLocationId: Int?  // If set, we're editing existing location

    init(playerId: Int, currentTime: Double, editingLocationId: Int? = nil) {
        self.playerId = playerId
        self.currentTime = currentTime
        self.editingLocationId = editingLocationId

        // If editing existing location, load its data
        if let locationId = editingLocationId {
            var data = FSTP_MemoryLocationData()
            // Find location index by ID
            let count = Int(FSTP_GetMemoryLocationsCount())
            for i in 0..<count {
                if FSTP_GetMemoryLocationData(Int32(i), &data) {
                    if data.id == locationId {
                        self.locationNumber = "\(data.id)"

                        // Safe conversion from C fixed-size char arrays
                        self.name = withUnsafeBytes(of: data.name) { ptr in
                            String(cString: ptr.bindMemory(to: CChar.self).baseAddress!)
                        }
                        self.comments = withUnsafeBytes(of: data.comments) { ptr in
                            String(cString: ptr.bindMemory(to: CChar.self).baseAddress!)
                        }
                        self.timecode = withUnsafeBytes(of: data.timecode_display) { ptr in
                            String(cString: ptr.bindMemory(to: CChar.self).baseAddress!)
                        }

                        self.recallZoom = data.recall_zoom
                        break
                    }
                }
            }
        } else {
            // New location: fill with defaults
            self.timecode = formatTimecode(seconds: currentTime)
            let nextId = Int(FSTP_GetMemoryLocationsCount()) + 1
            self.locationNumber = "\(nextId)"
        }
    }

    private func formatTimecode(seconds: Double) -> String {
        let hours = Int(seconds) / 3600
        let minutes = (Int(seconds) % 3600) / 60
        let secs = Int(seconds) % 60

        // Get actual video FPS
        var fps = GetInstanceVideoFPS(Int32(playerId))
        if fps <= 0 { fps = 25.0 } // fallback

        let frames = Int((seconds - floor(seconds)) * fps)
        return String(format: "%02d:%02d:%02d:%02d", hours, minutes, secs, frames)
    }

    func save() -> Bool {
        guard !name.isEmpty else { return false }

        let locationId = Int(locationNumber) ?? (Int(FSTP_GetMemoryLocationsCount()) + 1)

        // Parse timecode back to seconds
        let timecodeSeconds = parseTimecode(timecode) ?? currentTime

        // Get current zoom state (window_index == player_id)
        var zoomFactor: Float = 1.0
        var zoomCenterX: Float = 0.5
        var zoomCenterY: Float = 0.5

        if recallZoom, let zoomState = GetZoomState(Int32(playerId)) {
            if zoomState.pointee.enabled {
                zoomFactor = zoomState.pointee.factor
                zoomCenterX = zoomState.pointee.center_x
                zoomCenterY = zoomState.pointee.center_y
            }
        }

        let success: Bool
        if let _ = editingLocationId {
            // Update existing location
            success = FSTP_UpdateMemoryLocationFull(
                Int32(locationId),
                name,
                comments,
                timecodeSeconds,
                recallZoom,
                zoomFactor,
                zoomCenterX,
                zoomCenterY
            )
        } else {
            // Add new location
            success = FSTP_AddMemoryLocationWithZoom(
                Int32(playerId),
                Int32(locationId),
                name,
                comments,
                timecodeSeconds,
                recallZoom,
                zoomFactor,
                zoomCenterX,
                zoomCenterY
            )
        }

        return success
    }

    private func parseTimecode(_ tc: String) -> Double? {
        let components = tc.split(separator: ":").compactMap { Int($0) }
        guard components.count == 4 else { return nil }

        let hours = components[0]
        let minutes = components[1]
        let seconds = components[2]
        let frames = components[3]

        // Get actual video FPS
        var fps = GetInstanceVideoFPS(Int32(playerId))
        if fps <= 0 { fps = 25.0 } // fallback

        let totalSeconds = Double(hours * 3600 + minutes * 60 + seconds)
        let frameSeconds = Double(frames) / fps

        return totalSeconds + frameSeconds
    }
}

@available(macOS 13.0, *)
struct MemoryLocationDialog: View {
    @ObservedObject var viewModel: MemoryLocationViewModel
    @Environment(\.presentationMode) var presentationMode

    // Focus state for Tab navigation
    enum Field: Hashable {
        case number
        case timecode
        case name
        case comments
    }
    @FocusState private var focusedField: Field?

    var onClose: (() -> Void)?

    private let labelWidth: CGFloat = 88

    private var isEditing: Bool { viewModel.editingLocationId != nil }

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            content
            Divider()
            footer
        }
        .frame(width: 460)
        .background(Color(NSColor.windowBackgroundColor))
    }

    // MARK: Header — icon, title, and a prominent timecode badge
    private var header: some View {
        HStack(spacing: 14) {
            ZStack {
                Circle()
                    .fill(Color.accentColor.opacity(0.15))
                    .frame(width: 40, height: 40)
                Image(systemName: "mappin.and.ellipse")
                    .font(.system(size: 19, weight: .semibold))
                    .foregroundColor(.accentColor)
            }

            VStack(alignment: .leading, spacing: 2) {
                Text(isEditing ? "Edit Memory Location" : "New Memory Location")
                    .font(.system(size: 16, weight: .semibold))
                Text("Player \(viewModel.playerId + 1)  ·  #\(viewModel.locationNumber)")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
            }

            Spacer()

            VStack(alignment: .trailing, spacing: 2) {
                Text("TIMECODE")
                    .font(.system(size: 9, weight: .semibold))
                    .tracking(1.2)
                    .foregroundStyle(.secondary)
                Text(viewModel.timecode)
                    .font(.system(.title3, design: .monospaced).weight(.semibold))
                    .foregroundColor(.primary)
            }
        }
        .padding(.horizontal, 22)
        .padding(.vertical, 16)
        .background(.regularMaterial)
    }

    // MARK: Content — labelled fields
    private var content: some View {
        VStack(alignment: .leading, spacing: 14) {
            // Number + Timecode (compact, editable)
            HStack(spacing: 18) {
                labeledField("Number") {
                    TextField("", text: $viewModel.locationNumber)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 64)
                        .focused($focusedField, equals: .number)
                }
                Spacer()
                HStack(spacing: 10) {
                    Text("Timecode")
                        .frame(width: 70, alignment: .trailing)
                        .foregroundStyle(.secondary)
                    TextField("", text: $viewModel.timecode)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 120)
                        .font(.system(.body, design: .monospaced))
                        .focused($focusedField, equals: .timecode)
                }
            }

            labeledField("Name") {
                TextField("", text: $viewModel.name)
                    .textFieldStyle(.roundedBorder)
                    .focused($focusedField, equals: .name)
            }

            // Comments — single line so Tab moves on and Enter creates the marker
            labeledField("Comments") {
                TextField("", text: $viewModel.comments)
                    .textFieldStyle(.roundedBorder)
                    .focused($focusedField, equals: .comments)
            }
        }
        .padding(.horizontal, 22)
        .padding(.vertical, 18)
        .onAppear {
            // Start in the Name field
            focusedField = .name
        }
    }

    // MARK: Footer — recall-zoom toggle + actions
    private var footer: some View {
        HStack(spacing: 12) {
            Toggle(isOn: $viewModel.recallZoom) {
                Label("Recall zoom", systemImage: "viewfinder")
            }
            .toggleStyle(.checkbox)

            Spacer()

            Button("Cancel") {
                onClose?()
                NSApp.stopModal()
            }
            .keyboardShortcut(.cancelAction)
            .controlSize(.large)

            Button(isEditing ? "Save" : "Create") {
                if viewModel.save() {
                    // Keep the Memory Locations list window (if open) in sync
                    RefreshSwiftUIMemoryLocationsWindow()
                    onClose?()
                    NSApp.stopModal()
                }
            }
            .keyboardShortcut(.defaultAction)
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .disabled(viewModel.name.isEmpty)
        }
        .padding(.horizontal, 22)
        .padding(.vertical, 16)
        .background(.regularMaterial)
    }

    // MARK: Helper — a right-aligned label paired with a field
    @ViewBuilder
    private func labeledField<Content: View>(_ label: String,
                                             @ViewBuilder _ field: () -> Content) -> some View {
        HStack(spacing: 10) {
            Text(label)
                .frame(width: labelWidth, alignment: .trailing)
                .foregroundStyle(.secondary)
            field()
        }
    }
}

// MARK: - C Bridge Functions
@_cdecl("ShowSwiftUIMemoryLocationDialog")
public func ShowSwiftUIMemoryLocationDialog(_ playerId: Int32, _ currentTime: Double) {
    ShowSwiftUIMemoryLocationDialogEdit(playerId, currentTime, -1)
}

@_cdecl("ShowSwiftUIMemoryLocationDialogEdit")
public func ShowSwiftUIMemoryLocationDialogEdit(_ playerId: Int32, _ currentTime: Double, _ locationId: Int32) {
    if #available(macOS 13.0, *) {
        DispatchQueue.main.async {
            let editingId = locationId >= 0 ? Int(locationId) : nil
            let viewModel = MemoryLocationViewModel(
                playerId: Int(playerId),
                currentTime: currentTime,
                editingLocationId: editingId
            )

            var window: NSWindow?
            let onClose: () -> Void = {
                // Reset flag in C++
                OnMemoryLocationDialogClosedCallback()
            }

            let dialogView = MemoryLocationDialog(viewModel: viewModel, onClose: onClose)
            let hostingController = NSHostingController(rootView: dialogView)

            window = NSWindow(contentViewController: hostingController)
            window?.title = ""
            window?.styleMask = [.titled, .closable]
            window?.titlebarAppearsTransparent = true
            window?.titleVisibility = .hidden
            window?.isMovableByWindowBackground = true
            window?.isReleasedWhenClosed = false
            window?.center()

            // Make window modal - blocks all other windows
            if let w = window {
                NSApp.runModal(for: w)
                w.close()
            }
        }
    }
}
