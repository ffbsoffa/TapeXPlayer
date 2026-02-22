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

    var body: some View {
        VStack(spacing: 0) {
            // Header
            ZStack(alignment: .leading) {
                Color(NSColor.windowBackgroundColor)

                HStack(spacing: 8) {
                    Image(systemName: "mappin.circle.fill")
                        .font(.system(size: 18))
                        .foregroundColor(.accentColor)

                    Text(viewModel.editingLocationId != nil ? "Edit Memory Location" : "New Memory Location")
                        .font(.system(size: 16, weight: .semibold))

                    Spacer()
                }
                .padding(.leading, 20)
            }
            .frame(height: 50)

            Divider()

            // Content
            VStack(alignment: .leading, spacing: 16) {
                // Number and Timecode row
                HStack(alignment: .center, spacing: 20) {
                    HStack(spacing: 8) {
                        Text("Number:")
                            .frame(width: 60, alignment: .trailing)
                        TextField("", text: $viewModel.locationNumber)
                            .textFieldStyle(.roundedBorder)
                            .frame(width: 70)
                            .focused($focusedField, equals: .number)
                            .onSubmit {
                                focusedField = .timecode
                            }
                    }

                    Spacer()

                    HStack(spacing: 8) {
                        Text("Timecode:")
                            .frame(width: 70, alignment: .trailing)
                        TextField("", text: $viewModel.timecode)
                            .textFieldStyle(.roundedBorder)
                            .frame(width: 110)
                            .font(.system(.body, design: .monospaced))
                            .focused($focusedField, equals: .timecode)
                            .onSubmit {
                                focusedField = .name
                            }
                    }
                }

                // Name row
                HStack(spacing: 8) {
                    Text("Name:")
                        .frame(width: 60, alignment: .trailing)
                    TextField("", text: $viewModel.name)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 307)
                        .focused($focusedField, equals: .name)
                        .onSubmit {
                            focusedField = .comments
                        }
                }

                // Comments field (multiline)
                HStack(alignment: .top, spacing: 8) {
                    Text("Comments:")
                        .frame(width: 60, alignment: .trailing)
                        .padding(.top, 8)

                    ZStack(alignment: .topLeading) {
                        TextEditor(text: $viewModel.comments)
                            .font(.system(size: 12))
                            .frame(width: 307, height: 80)
                            .focused($focusedField, equals: .comments)
                    }
                    .border(Color.gray.opacity(0.3), width: 1)
                    .cornerRadius(4)
                }
            }
            .padding(.horizontal, 30)
            .padding(.vertical, 24)
            .onAppear {
                // Set initial focus to Name field
                focusedField = .name
            }

            Divider()

            // Buttons
            HStack(spacing: 12) {
                Toggle("Recall zoom settings", isOn: $viewModel.recallZoom)

                Spacer()

                Button("Cancel") {
                    onClose?()
                    NSApp.stopModal()
                }
                .keyboardShortcut(.cancelAction)

                Button("OK") {
                    if viewModel.save() {
                        onClose?()
                        NSApp.stopModal()
                    }
                }
                .keyboardShortcut(.defaultAction)
                .buttonStyle(.borderedProminent)
                .disabled(viewModel.name.isEmpty)
            }
            .padding(20)
        }
        .frame(width: 430, height: 340)
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
