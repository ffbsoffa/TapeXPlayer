import SwiftUI
import AppKit

// MARK: - Inspector View Model
@available(macOS 13.0, *)
class InspectorViewModel: ObservableObject {
    @Published var activeInstances: [PlayerInstanceInfo] = []
    @Published var selectedInstanceID: Int = -1
    @Published var selectedInstanceInfo: FileInfo? = nil

    private var updateTimer: Timer?

    struct PlayerInstanceInfo: Identifiable, Equatable {
        let id: Int
        let fileName: String

        static func == (lhs: PlayerInstanceInfo, rhs: PlayerInstanceInfo) -> Bool {
            return lhs.id == rhs.id && lhs.fileName == rhs.fileName
        }
    }

    struct FileInfo {
        let fileName: String
        let filePath: String
        let duration: Double
        let fps: Double
        let width: Int
        let height: Int
        let totalFrames: Int
        let currentPosition: Double
        let currentFrame: Int
        let audioSampleRate: Int
        let audioChannels: Int
        let audioCodecName: String
        let videoCodecName: String
    }

    init() {
        refreshInstances()
        startAutoRefresh()
    }

    deinit {
        stopAutoRefresh()
    }

    func refreshInstances() {
        var instances: [PlayerInstanceInfo] = []

        // Get list of active instances
        let count = GetActiveInstanceCount()
        if count > 0 {
            var instanceIDs = [Int32](repeating: -1, count: 3)
            var actualCount: Int32 = 0

            instanceIDs.withUnsafeMutableBufferPointer { idsPtr in
                withUnsafeMutablePointer(to: &actualCount) { countPtr in
                    GetActiveInstanceIDs(idsPtr.baseAddress, countPtr)
                }
            }

            for i in 0..<Int(actualCount) {
                let instanceID = Int(instanceIDs[i])
                if IsVideoLoadedInInstance(Int32(instanceID)) != 0 {
                    if let fileNameCStr = GetInstanceFileName(Int32(instanceID)) {
                        let fileName = String(cString: fileNameCStr)
                        instances.append(PlayerInstanceInfo(id: instanceID, fileName: fileName))
                    }
                }
            }
        }

        activeInstances = instances

        // If currently selected instance is no longer active, clear selection
        if selectedInstanceID != -1 && !instances.contains(where: { $0.id == selectedInstanceID }) {
            selectedInstanceID = -1
            selectedInstanceInfo = nil
        }

        // Auto-select first instance if nothing selected
        if selectedInstanceID == -1 && !instances.isEmpty {
            selectedInstanceID = instances[0].id
            loadInstanceInfo(instanceID: selectedInstanceID)
        } else if selectedInstanceID != -1 {
            // Refresh current selection
            loadInstanceInfo(instanceID: selectedInstanceID)
        }
    }

    func loadInstanceInfo(instanceID: Int) {
        guard IsPlayerInstanceActive(Int32(instanceID)) != 0 else {
            selectedInstanceInfo = nil
            return
        }

        guard IsVideoLoadedInInstance(Int32(instanceID)) != 0 else {
            selectedInstanceInfo = nil
            return
        }

        let fileName = GetInstanceFileName(Int32(instanceID)).map { String(cString: $0) } ?? "Unknown"
        let filePath = GetInstanceFilePath(Int32(instanceID)).map { String(cString: $0) } ?? ""
        let duration = GetInstanceDuration(Int32(instanceID))
        let fps = GetInstanceVideoFPS(Int32(instanceID))
        let width = GetInstanceVideoWidth(Int32(instanceID))
        let height = GetInstanceVideoHeight(Int32(instanceID))
        let totalFrames = GetInstanceTotalFrames(Int32(instanceID))
        let position = GetInstancePosition(Int32(instanceID))
        let currentFrame = Int(position * fps)
        let sampleRate = GetInstanceAudioSampleRate(Int32(instanceID))
        let channels = GetInstanceAudioChannels(Int32(instanceID))
        let audioCodec = GetInstanceAudioCodecName(Int32(instanceID)).map { String(cString: $0) } ?? "Unknown"
        let videoCodec = GetInstanceVideoCodecName(Int32(instanceID)).map { String(cString: $0) } ?? "Unknown"

        selectedInstanceInfo = FileInfo(
            fileName: fileName,
            filePath: filePath,
            duration: duration,
            fps: fps,
            width: Int(width),
            height: Int(height),
            totalFrames: Int(totalFrames),
            currentPosition: position,
            currentFrame: currentFrame,
            audioSampleRate: Int(sampleRate),
            audioChannels: Int(channels),
            audioCodecName: audioCodec,
            videoCodecName: videoCodec
        )
    }

    func selectInstance(id: Int) {
        selectedInstanceID = id
        loadInstanceInfo(instanceID: id)
    }

    private func startAutoRefresh() {
        updateTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            self?.refreshInstances()
        }
    }

    private func stopAutoRefresh() {
        updateTimer?.invalidate()
        updateTimer = nil
    }

    func formatTime(_ seconds: Double) -> String {
        let hours = Int(seconds) / 3600
        let minutes = (Int(seconds) % 3600) / 60
        let secs = Int(seconds) % 60
        let frames = Int((seconds - Double(Int(seconds))) * (selectedInstanceInfo?.fps ?? 25.0))

        if hours > 0 {
            return String(format: "%02d:%02d:%02d:%02d", hours, minutes, secs, frames)
        } else {
            return String(format: "%02d:%02d:%02d", minutes, secs, frames)
        }
    }
}

// MARK: - Inspector Content View
@available(macOS 13.0, *)
struct InspectorContentView: View {
    @ObservedObject var viewModel: InspectorViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            if viewModel.activeInstances.isEmpty {
                // No instances loaded
                VStack(spacing: 8) {
                    Image(systemName: "film.stack")
                        .font(.system(size: 32))
                        .foregroundColor(.secondary)
                    Text("No Video Loaded")
                        .font(.caption)
                        .fontWeight(.semibold)
                        .foregroundColor(.secondary)
                    Text("Open a file to see its properties")
                        .font(.caption2)
                        .foregroundColor(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                // Instance selector
                if viewModel.activeInstances.count > 1 {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Player")
                            .font(.caption2)
                            .foregroundColor(.secondary)

                        Picker("", selection: $viewModel.selectedInstanceID) {
                            ForEach(viewModel.activeInstances) { instance in
                                Text("\(instance.id + 1): \(instance.fileName)")
                                    .font(.caption)
                                    .tag(instance.id)
                            }
                        }
                        .font(.caption)
                        .onChange(of: viewModel.selectedInstanceID) { newValue in
                            viewModel.selectInstance(id: newValue)
                        }
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)

                    Divider()
                }

                // File properties
                if let info = viewModel.selectedInstanceInfo {
                    ScrollView {
                        VStack(alignment: .leading, spacing: 8) {
                            // File Information
                            Section {
                                PropertyRow(label: "Name", value: info.fileName)
                            } header: {
                                SectionHeader(title: "File", icon: "doc.fill")
                            }

                            Divider().padding(.vertical, 4)

                            // Video Properties
                            Section {
                                PropertyRow(label: "Codec", value: info.videoCodecName)
                                PropertyRow(label: "Resolution", value: "\(info.width) × \(info.height)")
                                PropertyRow(label: "FPS", value: String(format: "%.2f", info.fps))
                                PropertyRow(label: "Frames", value: "\(info.totalFrames)")
                            } header: {
                                SectionHeader(title: "Video", icon: "film.fill")
                            }

                            Divider().padding(.vertical, 4)

                            // Audio Properties
                            Section {
                                PropertyRow(label: "Codec", value: info.audioCodecName)
                                PropertyRow(label: "Sample Rate", value: "\(info.audioSampleRate) Hz")
                                PropertyRow(label: "Channels", value: info.audioChannels == 2 ? "Stereo" : "\(info.audioChannels)ch")
                            } header: {
                                SectionHeader(title: "Audio", icon: "waveform")
                            }

                            Divider().padding(.vertical, 4)

                            // Playback Position
                            Section {
                                PropertyRow(label: "Time", value: viewModel.formatTime(info.currentPosition))
                                PropertyRow(label: "Frame", value: "\(info.currentFrame)")
                            } header: {
                                SectionHeader(title: "Playback", icon: "play.circle.fill")
                            }
                        }
                        .padding(12)
                    }
                } else {
                    VStack(spacing: 8) {
                        Image(systemName: "exclamationmark.triangle")
                            .font(.system(size: 32))
                            .foregroundColor(.orange)
                        Text("Unable to Load Info")
                            .font(.caption)
                            .fontWeight(.semibold)
                            .foregroundColor(.secondary)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
        }
        .frame(minWidth: 280, idealWidth: 320, maxWidth: .infinity, minHeight: 250, maxHeight: .infinity)
    }
}

// MARK: - Property Row
@available(macOS 13.0, *)
struct PropertyRow: View {
    let label: String
    let value: String
    var monospaced: Bool = false

    var body: some View {
        HStack(alignment: .top, spacing: 6) {
            Text(label + ":")
                .font(.caption)
                .foregroundColor(.secondary)
                .frame(width: 75, alignment: .trailing)

            if monospaced {
                Text(value)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            } else {
                Text(value)
                    .font(.caption)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }
}

// MARK: - Section Header
@available(macOS 13.0, *)
struct SectionHeader: View {
    let title: String
    let icon: String

    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: icon)
                .font(.caption2)
                .foregroundColor(.accentColor)
            Text(title)
                .font(.caption)
                .fontWeight(.semibold)
        }
        .padding(.bottom, 2)
    }
}

// MARK: - Main Inspector Window
@available(macOS 13.0, *)
struct InspectorView: View {
    @ObservedObject var viewModel: InspectorViewModel
    @Environment(\.presentationMode) var presentationMode

    var body: some View {
        InspectorContentView(viewModel: viewModel)
            .frame(width: 320, height: 420)
    }
}

// MARK: - Inspector Window Controller
@available(macOS 13.0, *)
class InspectorWindowController: NSWindowController {
    private var viewModel: InspectorViewModel?

    convenience init() {
        let viewModel = InspectorViewModel()
        let hostingController = NSHostingController(rootView: InspectorView(viewModel: viewModel))

        let window = NSPanel(contentViewController: hostingController)
        window.title = "Inspector"
        window.styleMask = [.titled, .closable, .resizable, .utilityWindow]
        window.setContentSize(NSSize(width: 320, height: 420))
        window.minSize = NSSize(width: 280, height: 350)
        window.level = .floating
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        window.isReleasedWhenClosed = false

        self.init(window: window)
        self.viewModel = viewModel
    }
}

// MARK: - C Bridge Function
private var sharedInspectorWindow: InspectorWindowController?

@_cdecl("ShowSwiftUIInspector")
func ShowSwiftUIInspector() {
    if #available(macOS 13.0, *) {
        DispatchQueue.main.async {
            if sharedInspectorWindow == nil {
                sharedInspectorWindow = InspectorWindowController()
            }
            sharedInspectorWindow?.showWindow(nil)
            sharedInspectorWindow?.window?.makeKeyAndOrderFront(nil)
        }
    } else {
        print("SwiftUI inspector requires macOS 13.0 Ventura or later")
    }
}

@_cdecl("HideSwiftUIInspector")
func HideSwiftUIInspector() {
    if #available(macOS 13.0, *) {
        DispatchQueue.main.async {
            sharedInspectorWindow?.window?.orderOut(nil)
        }
    }
}

@_cdecl("ToggleSwiftUIInspector")
func ToggleSwiftUIInspector() {
    if #available(macOS 13.0, *) {
        DispatchQueue.main.async {
            if sharedInspectorWindow == nil {
                sharedInspectorWindow = InspectorWindowController()
            }

            if let window = sharedInspectorWindow?.window {
                if window.isVisible {
                    window.orderOut(nil)
                } else {
                    window.makeKeyAndOrderFront(nil)
                }
            }
        }
    }
}
