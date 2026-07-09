import SwiftUI
import AppKit

// MARK: - Settings View Model
@available(macOS 13.0, *)
class SettingsViewModel: ObservableObject {
    // Audio Settings
    @Published var audioDeviceIndex: Int = -1
    @Published var masterVolume: Double = 1.0
    @Published var bufferSize: Int = 1024
    @Published var volumeDuckingEnabled: Bool = true

    // Video & Sync Settings
    @Published var frameOffset: Int = 0
    @Published var autoFreezeInactive: Bool = true
    @Published var betacamEffectEnabled: Bool = false
    @Published var ytDlpEnabled: Bool = false

    // MIDI Settings
    @Published var midiEnabled: Bool = false
    @Published var midiInputPort: Int = -1
    @Published var midiOutputPort: Int = -1

    // Developer/Debug Settings
    @Published var showDecoderStatus: Bool = false

    // Presentation Settings
    @Published var presentationOutputMode: Int = 0     // 0 = external display, 1 = separate window
    @Published var presentationDisplayIndex: Int = -1  // -1 = auto (first external)
    @Published var presentationFollowFocus: Bool = true
    @Published var presentationPinnedPlayer: Int = 0

    // Power management

    // Available devices
    @Published var audioDevices: [(index: Int, name: String)] = []
    @Published var midiInputDevices: [String] = []
    @Published var midiOutputDevices: [String] = []
    @Published var presentationDisplays: [(index: Int, name: String)] = []
    @Published var presentationActivePlayers: [Int] = []

    let bufferSizes = [512, 1024, 2048, 4096]
    let ytDlpAvailable: Bool

    init() {
        ytDlpAvailable = FSTP_YTDLP_IsAvailable() != 0
        loadSettings()
        loadDevices()
    }

    func loadSettings() {
        let settings = GetSettings()
        if let s = settings {
            audioDeviceIndex = Int(s.pointee.audio_device_index)
            masterVolume = Double(s.pointee.audio_master_volume)
            bufferSize = Int(s.pointee.audio_buffer_size)
            volumeDuckingEnabled = s.pointee.audio_volume_ducking_enabled != 0
            frameOffset = Int(s.pointee.frame_offset)
            autoFreezeInactive = s.pointee.auto_freeze_inactive != 0
            betacamEffectEnabled = s.pointee.betacam_effect_enabled != 0
            ytDlpEnabled = s.pointee.yt_dlp_extension_enabled != 0
            midiEnabled = s.pointee.midi_enabled != 0
            midiInputPort = Int(s.pointee.midi_input_port)
            midiOutputPort = Int(s.pointee.midi_output_port)
            showDecoderStatus = s.pointee.show_decoder_status != 0
            presentationOutputMode = Int(s.pointee.presentation_output_mode)
            presentationDisplayIndex = Int(s.pointee.presentation_display_index)
            presentationFollowFocus = s.pointee.presentation_follow_focus != 0
            presentationPinnedPlayer = Int(s.pointee.presentation_pinned_player)
        }

        if !ytDlpAvailable {
            ytDlpEnabled = false
        }
    }

    func loadDevices() {
        // Load audio devices using PortAudio
        let deviceCount = Pa_GetDeviceCount()
        audioDevices = []

        // Add default device
        audioDevices.append((index: -1, name: "Default Audio Device"))

        for i in 0..<deviceCount {
            if let deviceInfo = Pa_GetDeviceInfo(i) {
                if deviceInfo.pointee.maxOutputChannels > 0 {
                    let name = String(cString: deviceInfo.pointee.name)
                    audioDevices.append((index: Int(i), name: name))
                }
            }
        }

        // Load MIDI devices
        let midiInputCount = GetMIDIInputDeviceCount()
        midiInputDevices = ["(None)"]
        for i in 0..<midiInputCount {
            if let deviceName = GetMIDIInputDeviceName(Int32(i)) {
                midiInputDevices.append(String(cString: deviceName))
            }
        }

        let midiOutputCount = GetMIDIOutputDeviceCount()
        midiOutputDevices = ["(None)"]
        for i in 0..<midiOutputCount {
            if let deviceName = GetMIDIOutputDeviceName(Int32(i)) {
                midiOutputDevices.append(String(cString: deviceName))
            }
        }

        // Presentation displays (SDL indices — match the window code 1:1)
        let displayCount = FSTP_GetPresentationDisplayCount()
        presentationDisplays = []
        for i in 0..<displayCount {
            let name = FSTP_GetPresentationDisplayName(i).map { String(cString: $0) } ?? "Display"
            presentationDisplays.append((index: Int(i), name: "Display \(i): \(name)"))
        }

        // Active players (for the pin picker)
        var ids = [Int32](repeating: 0, count: 8)
        var count: Int32 = 0
        GetActiveInstanceIDs(&ids, &count)
        presentationActivePlayers = (0..<Int(max(0, count))).map { Int(ids[$0]) }
    }

    func saveSettings() -> Bool {
        guard let settings = GetSettings() else { return false }

        var bufferSizeChanged = false

        // Check if buffer size changed
        if Int(settings.pointee.audio_buffer_size) != bufferSize {
            bufferSizeChanged = true
        }

        // Update settings
        settings.pointee.audio_device_index = Int32(audioDeviceIndex)
        settings.pointee.audio_master_volume = Float(masterVolume)
        settings.pointee.audio_buffer_size = Int32(bufferSize)
        settings.pointee.audio_volume_ducking_enabled = volumeDuckingEnabled ? 1 : 0
        settings.pointee.frame_offset = Int32(frameOffset)
        settings.pointee.auto_freeze_inactive = autoFreezeInactive ? 1 : 0
        settings.pointee.betacam_effect_enabled = betacamEffectEnabled ? 1 : 0
        settings.pointee.yt_dlp_extension_enabled = ytDlpEnabled ? 1 : 0
        settings.pointee.midi_enabled = midiEnabled ? 1 : 0
        settings.pointee.midi_input_port = Int32(midiInputPort)
        settings.pointee.midi_output_port = Int32(midiOutputPort)
        settings.pointee.show_decoder_status = showDecoderStatus ? 1 : 0
        settings.pointee.presentation_output_mode = Int32(presentationOutputMode)
        settings.pointee.presentation_display_index = Int32(presentationDisplayIndex)
        settings.pointee.presentation_follow_focus = presentationFollowFocus ? 1 : 0
        settings.pointee.presentation_pinned_player = Int32(presentationPinnedPlayer)

        // Save to file
        SaveSettings()

        // Apply settings
        ApplyAudioSettings()
        ApplyMIDISettings()
        SetYTDLPExtensionEnabled(ytDlpEnabled ? 1 : 0)
        InitToolsMenu()
        SetBetacamEffectEnabled(betacamEffectEnabled ? 1 : 0)
        // If presentation is running, move/retarget it live to match the new choice.
        FSTP_ReapplyPresentationIfActive()

        return bufferSizeChanged
    }

    func resetToDefaults() {
        ResetSettingsToDefault()
        loadSettings()
        SetYTDLPExtensionEnabled(ytDlpEnabled ? 1 : 0)
        InitToolsMenu()
    }
}

// MARK: - Audio Settings Tab
@available(macOS 13.0, *)
struct AudioSettingsView: View {
    @ObservedObject var viewModel: SettingsViewModel

    var body: some View {
        Form {
            Section(header: Text("Audio Device").font(.headline)) {
                Picker("Output Device:", selection: $viewModel.audioDeviceIndex) {
                    ForEach(viewModel.audioDevices, id: \.index) { device in
                        Text(device.name).tag(device.index)
                    }
                }
            }

            Section(header: Text("Volume").font(.headline)) {
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Text("Master Volume:")
                        Spacer()
                        Text("\(Int(viewModel.masterVolume * 100))%")
                            .foregroundColor(.secondary)
                            .monospacedDigit()
                    }
                    Slider(value: $viewModel.masterVolume, in: 0...1)
                }

                Divider()

                VStack(alignment: .leading, spacing: 4) {
                    Toggle("Auto-Reduce Volume at High Speeds", isOn: $viewModel.volumeDuckingEnabled)

                    Text("Protects your ears during shuttle (6x: fade starts, 12x: -24dB, 32x: -40dB)")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }

            Section(header: Text("Performance").font(.headline)) {
                Picker("Buffer Size:", selection: $viewModel.bufferSize) {
                    ForEach(viewModel.bufferSizes, id: \.self) { size in
                        Text("\(size) samples").tag(size)
                    }
                }

                Text("Note: Buffer size changes require application restart")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
        }
        .formStyle(.grouped)
        .padding(20)
    }
}

// MARK: - Video & Sync Settings Tab
@available(macOS 13.0, *)
struct VideoSyncSettingsView: View {
    @ObservedObject var viewModel: SettingsViewModel

    var body: some View {
        Form {
            Section(header: Text("Display Synchronization").font(.headline)) {
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Text("Frame Offset:")

                        Spacer()

                        // Minus button
                        Button(action: {
                            if viewModel.frameOffset > -10 {
                                viewModel.frameOffset -= 1
                            }
                        }) {
                            Image(systemName: "minus.circle.fill")
                                .foregroundColor(viewModel.frameOffset > -10 ? .accentColor : .gray)
                        }
                        .buttonStyle(.borderless)
                        .disabled(viewModel.frameOffset <= -10)

                        // Value
                        Text("\(viewModel.frameOffset)")
                            .font(.system(.body, design: .monospaced))
                            .frame(minWidth: 30, alignment: .center)

                        // Plus button
                        Button(action: {
                            if viewModel.frameOffset < 10 {
                                viewModel.frameOffset += 1
                            }
                        }) {
                            Image(systemName: "plus.circle.fill")
                                .foregroundColor(viewModel.frameOffset < 10 ? .accentColor : .gray)
                        }
                        .buttonStyle(.borderless)
                        .disabled(viewModel.frameOffset >= 10)

                        Text("frames")
                            .foregroundColor(.secondary)
                    }

                    Text("Compensate for display lag (-10 to +10 frames)")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }

            Section(header: Text("Multi-Instance Performance").font(.headline)) {
                Toggle("Auto-freeze inactive players", isOn: $viewModel.autoFreezeInactive)

                Text("Prevents forgotten players from consuming resources")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }


            Section(header: Text("Visual Effects").font(.headline)) {
                Toggle("Enable Betacam tape artefact emulation", isOn: $viewModel.betacamEffectEnabled)

                Text("Adds rewind/fast-forward tape jitter. May impact performance on slower GPUs.")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Section(header: Text("Developer/Debug").font(.headline)) {
                Toggle("Show Decoder Status", isOn: $viewModel.showDecoderStatus)

                Text("Displays decoded frames indicator at the top of the screen. Useful for debugging decoder performance.")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .formStyle(.grouped)
        .padding(20)
    }
}

// MARK: - Presentation Settings Tab
@available(macOS 13.0, *)
struct PresentationSettingsView: View {
    @ObservedObject var viewModel: SettingsViewModel

    private var externalDisplays: [(index: Int, name: String)] {
        viewModel.presentationDisplays.filter { $0.index >= 1 }
    }

    var body: some View {
        Form {
            Section(header: Text("Output").font(.headline)) {
                Picker("Send picture to:", selection: $viewModel.presentationOutputMode) {
                    Text("External display").tag(0)
                    Text("Separate window").tag(1)
                }
                .pickerStyle(.segmented)

                if viewModel.presentationOutputMode == 0 {
                    Picker("Display:", selection: $viewModel.presentationDisplayIndex) {
                        Text("Auto (first external)").tag(-1)
                        ForEach(externalDisplays, id: \.index) { d in
                            Text(d.name).tag(d.index)
                        }
                    }
                    if externalDisplays.isEmpty {
                        Text("No external display connected — presentation will open in a separate window until one is attached.")
                            .font(.caption).foregroundColor(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                } else {
                    Text("A separate, movable window. Drag it to any screen, or share it in a video call.")
                        .font(.caption).foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }

            Section(header: Text("Source").font(.headline)) {
                Picker("Driven by:", selection: $viewModel.presentationFollowFocus) {
                    Text("Focused player").tag(true)
                    Text("Pinned player").tag(false)
                }
                .pickerStyle(.segmented)

                if !viewModel.presentationFollowFocus {
                    Picker("Pin to:", selection: $viewModel.presentationPinnedPlayer) {
                        if viewModel.presentationActivePlayers.isEmpty {
                            Text("Player 1").tag(0)
                        } else {
                            ForEach(viewModel.presentationActivePlayers, id: \.self) { pid in
                                Text("Player \(pid + 1)").tag(pid)
                            }
                        }
                    }
                }

                Text(viewModel.presentationFollowFocus
                     ? "The presentation screen follows whichever player window you’re working in."
                     : "The presentation screen stays locked to one player, regardless of focus.")
                    .font(.caption).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Section(header: Text("Clean output").font(.headline)) {
                Text("The presentation screen shows only the picture — never the OSD, decoder indicators, or the cursor. (Betacam hardware-protection principle.)")
                    .font(.caption).foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Text("Toggle presentation with ⇧P, or Tools ▸ Presentation Mode.")
                    .font(.caption).foregroundColor(.secondary)
            }
        }
        .formStyle(.grouped)
        .padding(20)
    }
}

// MARK: - MIDI Settings Tab
@available(macOS 13.0, *)
struct MIDISettingsView: View {
    @ObservedObject var viewModel: SettingsViewModel

    var body: some View {
        Form {
            Section(header: Text("MIDI Controller").font(.headline)) {
                Toggle("Enable MIDI Controller", isOn: $viewModel.midiEnabled)
            }

            Section(header: Text("MIDI Ports").font(.headline)) {
                Picker("Input Port:", selection: $viewModel.midiInputPort) {
                    ForEach(0..<viewModel.midiInputDevices.count, id: \.self) { index in
                        Text(viewModel.midiInputDevices[index]).tag(index - 1)
                    }
                }
                .disabled(!viewModel.midiEnabled)

                Picker("Output Port:", selection: $viewModel.midiOutputPort) {
                    ForEach(0..<viewModel.midiOutputDevices.count, id: \.self) { index in
                        Text(viewModel.midiOutputDevices[index]).tag(index - 1)
                    }
                }
                .disabled(!viewModel.midiEnabled)
            }

            Section(header: Text("Protocol Information").font(.headline)) {
                VStack(alignment: .leading, spacing: 8) {
                    HStack(spacing: 4) {
                        Image(systemName: "info.circle.fill")
                            .foregroundColor(.blue)
                            .font(.caption)
                        Text("Mackie Control Protocol")
                            .font(.system(size: 11, weight: .semibold))
                    }

                    Divider()
                        .padding(.vertical, 4)

                    VStack(alignment: .leading, spacing: 6) {
                        Text("⚠️ Controller support is in development")
                            .font(.caption)
                            .foregroundColor(.orange)
                            .fontWeight(.medium)

                        Text("Some features may not work correctly. Best compatibility with Behringer X-Touch One and similar Mackie Control compatible devices.")
                            .font(.caption)
                            .foregroundColor(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    .padding(.top, 4)
                }
            }
        }
        .formStyle(.grouped)
        .padding(20)
    }
}

// MARK: - Cache & Data Settings Tab
@available(macOS 13.0, *)
struct CacheDataSettingsView: View {
    @State private var proxyCachePath: String = ""
    @State private var memoryCachePath: String = ""
    @State private var proxyCacheSize: Int = 0
    @State private var proxyFilesCount: Int = 0
    @State private var hasActivePlayers: Bool = false
    @State private var showClearConfirmation: Bool = false
    @State private var memoryLocationsFiles: [String] = []
    @State private var showClearMemoryLocationsConfirmation: Bool = false

    var body: some View {
        Form {
            Section(header: Text("Proxy Video Cache").font(.headline)) {
                VStack(alignment: .leading, spacing: 12) {
                    HStack {
                        Text("Location:")
                            .frame(width: 90, alignment: .leading)
                        Text(proxyCachePath)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                            .foregroundColor(.secondary)
                        Spacer()
                        Button("Show in Finder") {
                            NSWorkspace.shared.selectFile(nil, inFileViewerRootedAtPath: proxyCachePath)
                        }
                        .buttonStyle(.borderless)
                    }

                    HStack {
                        Text("Cache Size:")
                            .frame(width: 90, alignment: .leading)
                        Text("\(proxyCacheSize) MB (\(proxyFilesCount) files)")
                            .font(.system(.body, design: .monospaced))
                        Spacer()
                    }

                    HStack {
                        Spacer()
                        if hasActivePlayers {
                            Button("Clear Inactive") {
                                FSTP_ClearProxyCache(true)
                                refreshCacheInfo()
                            }
                            .help("Remove proxy files except for currently loaded videos")
                        }
                        Button("Clear All") {
                            showClearConfirmation = true
                        }
                        .disabled(proxyFilesCount == 0)
                    }
                }
            }

            Section(header: Text("Memory Locations").font(.headline)) {
                VStack(alignment: .leading, spacing: 12) {
                    HStack {
                        Text("Location:")
                            .frame(width: 90, alignment: .leading)
                        Text(memoryCachePath)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                            .foregroundColor(.secondary)
                        Spacer()
                        Button("Show in Finder") {
                            NSWorkspace.shared.selectFile(nil, inFileViewerRootedAtPath: memoryCachePath)
                        }
                        .buttonStyle(.borderless)
                    }

                    HStack {
                        Text("Saved Data:")
                            .frame(width: 90, alignment: .leading)
                        Text("\(memoryLocationsFiles.count) video files")
                            .font(.system(.body, design: .monospaced))
                        Spacer()
                    }

                    if !memoryLocationsFiles.isEmpty {
                        Divider()
                            .padding(.vertical, 4)

                        VStack(alignment: .leading, spacing: 4) {
                            Text("Files with Memory Locations:")
                                .font(.caption)
                                .foregroundColor(.secondary)

                            ScrollView {
                                VStack(alignment: .leading, spacing: 2) {
                                    ForEach(memoryLocationsFiles, id: \.self) { filename in
                                        Text("• \(filename)")
                                            .font(.system(.caption, design: .monospaced))
                                            .foregroundColor(.primary)
                                    }
                                }
                            }
                            .frame(maxHeight: 120)
                        }
                    }

                    HStack {
                        Spacer()
                        Button("Clear All") {
                            showClearMemoryLocationsConfirmation = true
                        }
                        .disabled(memoryLocationsFiles.isEmpty)
                    }

                    Text("Memory Locations are saved per-video like browser cookies")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
        }
        .formStyle(.grouped)
        .padding(20)
        .onAppear {
            refreshCacheInfo()
        }
        .alert("Clear All Proxy Cache?", isPresented: $showClearConfirmation) {
            Button("Cancel", role: .cancel) { }
            Button("Clear", role: .destructive) {
                FSTP_ClearProxyCache(false)
                refreshCacheInfo()
            }
        } message: {
            Text("This will delete all proxy video files (\(proxyFilesCount) files, \(proxyCacheSize) MB). This cannot be undone.")
        }
        .alert("Clear All Memory Locations?", isPresented: $showClearMemoryLocationsConfirmation) {
            Button("Cancel", role: .cancel) { }
            Button("Clear", role: .destructive) {
                FSTP_ClearAllMemoryLocations()
                refreshCacheInfo()
            }
        } message: {
            Text("This will delete all saved Memory Locations for \(memoryLocationsFiles.count) video files. This cannot be undone.")
        }
    }

    private func refreshCacheInfo() {
        if let path = FSTP_GetProxyCachePath() {
            proxyCachePath = String(cString: path)
        }
        if let path = FSTP_GetMemoryLocationsCachePath() {
            memoryCachePath = String(cString: path) + "/memory_locations"
        }
        proxyCacheSize = Int(FSTP_GetProxyCacheSize())
        proxyFilesCount = Int(FSTP_GetProxyFilesCount())
        hasActivePlayers = FSTP_IsAnyPlayerActive()

        // Load Memory Locations files list
        memoryLocationsFiles.removeAll()
        let count = Int(FSTP_GetMemoryLocationsFilesCount())
        for i in 0..<count {
            if let filename = FSTP_GetMemoryLocationsFileName(Int32(i)) {
                memoryLocationsFiles.append(String(cString: filename))
            }
        }
    }
}

// MARK: - Extensions Settings Tab
@available(macOS 13.0, *)
struct ExtensionsSettingsView: View {
    @ObservedObject var viewModel: SettingsViewModel
    private let language = String(cString: GetExtensionLanguage())

    var body: some View {
        Form {
            Section(header: Text("Extension Language").font(.headline)) {
                Text("TapeXPlayer extensions are scripted in \(language) (.lua) files.")
                    .font(.body)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Section(header: Text("Status").font(.headline)) {
                Toggle("Enable yt-dlp network downloader", isOn: $viewModel.ytDlpEnabled)
                    .disabled(!viewModel.ytDlpAvailable)

                if !viewModel.ytDlpAvailable {
                    Text("yt-dlp not detected. Install it (e.g. via Homebrew) to enable network downloading.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }

                Text("Extension loading and management tools are being prepared. This area will expand as the Lua pipeline evolves.")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .formStyle(.grouped)
        .padding(20)
    }
}

// MARK: - Settings Navigation Item
@available(macOS 13.0, *)
enum SettingsPage: String, CaseIterable {
    case audio = "Audio"
    case videoSync = "Video & Sync"
    case presentation = "Presentation"
    case keyboard = "Keyboard"
    case midi = "MIDI"
    case cacheData = "Cache & Data"
    case extensions = "Extensions"

    var icon: String {
        switch self {
        case .audio: return "speaker.wave.2.fill"
        case .videoSync: return "tv.fill"
        case .presentation: return "rectangle.on.rectangle"
        case .keyboard: return "keyboard"
        case .midi: return "pianokeys"
        case .cacheData: return "externaldrive.fill"
        case .extensions: return "puzzlepiece"
        }
    }
}

// MARK: - Main Settings Window
@available(macOS 13.0, *)
struct SettingsView: View {
    @ObservedObject var viewModel: SettingsViewModel
    @Environment(\.presentationMode) var presentationMode
    @State private var showRestartAlert = false
    @State private var selectedPage: SettingsPage = .audio

    var body: some View {
        NavigationView {
            // Sidebar
            List(selection: $selectedPage) {
                ForEach(SettingsPage.allCases, id: \.self) { page in
                    Label(page.rawValue, systemImage: page.icon)
                        .tag(page)
                }
            }
            .listStyle(SidebarListStyle())
            .frame(minWidth: 150, idealWidth: 180, maxWidth: 200)

            // Content
            VStack(spacing: 0) {
                // Settings content based on selection
                Group {
                    switch selectedPage {
                    case .audio:
                        AudioSettingsView(viewModel: viewModel)
                    case .videoSync:
                        VideoSyncSettingsView(viewModel: viewModel)
                    case .presentation:
                        PresentationSettingsView(viewModel: viewModel)
                    case .keyboard:
                        KeyboardSettingsView()
                    case .midi:
                        MIDISettingsView(viewModel: viewModel)
                    case .cacheData:
                        CacheDataSettingsView()
                    case .extensions:
                        ExtensionsSettingsView(viewModel: viewModel)
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)

                Divider()

                // Buttons
                HStack {
                    Button("Reset to Defaults") {
                        viewModel.resetToDefaults()
                    }

                    Spacer()

                    Button("Cancel") {
                        presentationMode.wrappedValue.dismiss()
                    }
                    .keyboardShortcut(.cancelAction)

                    Button("OK") {
                        let bufferChanged = viewModel.saveSettings()

                        if bufferChanged {
                            showRestartAlert = true
                        } else {
                            presentationMode.wrappedValue.dismiss()
                        }
                    }
                    .keyboardShortcut(.defaultAction)
                }
                .padding(.horizontal, 20)
                .padding(.vertical, 16)
            }
            .frame(minWidth: 500, idealWidth: 600, maxWidth: .infinity)
        }
        .frame(width: 750, height: 500)
        .alert(isPresented: $showRestartAlert) {
            Alert(
                title: Text("Restart Required"),
                message: Text("Buffer size has been changed. Please restart TapeXPlayer for changes to take effect."),
                dismissButton: .default(Text("OK")) {
                    presentationMode.wrappedValue.dismiss()
                }
            )
        }
    }
}

// MARK: - Settings Window Controller
@available(macOS 13.0, *)
class SettingsWindowController: NSWindowController {
    convenience init() {
        let hostingController = NSHostingController(rootView: SettingsView(viewModel: SettingsViewModel()))

        let window = NSWindow(contentViewController: hostingController)
        window.title = "Settings"
        window.styleMask = [.titled, .closable, .resizable]
        window.setContentSize(NSSize(width: 750, height: 500))
        window.minSize = NSSize(width: 650, height: 450)
        window.center()
        window.isReleasedWhenClosed = false

        self.init(window: window)
    }
}

// MARK: - C Bridge Function
private var sharedSettingsWindow: SettingsWindowController?

@_cdecl("ShowSwiftUISettingsDialog")
func ShowSwiftUISettingsDialog() {
    if #available(macOS 13.0, *) {
        DispatchQueue.main.async {
            if sharedSettingsWindow == nil {
                sharedSettingsWindow = SettingsWindowController()
            }
            sharedSettingsWindow?.showWindow(nil)
            sharedSettingsWindow?.window?.makeKeyAndOrderFront(nil)
            NSApp.activate(ignoringOtherApps: true)
        }
    } else {
        print("SwiftUI settings require macOS 13.0 Ventura or later")
    }
}

// MARK: - Keyboard Shortcuts page
@available(macOS 13.0, *)
final class KeyboardSettingsModel: ObservableObject {
    struct ActionItem: Identifiable {
        let id: Int          // action_id
        let name: String
        let group: String
        let editable: Bool
        var keycode: Int
        var mods: Int
    }

    @Published var items: [ActionItem] = []
    @Published var capturingAction: Int? = nil
    @Published var conflictMessage: String? = nil

    private var monitor: Any?

    init() { reload() }
    deinit { stopCapture() }

    func reload() {
        var newItems: [ActionItem] = []
        let n = Int(FSTP_KB_GetActionCount())
        for i in 0..<n {
            let aid = Int(FSTP_KB_GetActionIdByIndex(Int32(i)))
            if aid == 0 { continue }
            newItems.append(ActionItem(
                id: aid,
                name: String(cString: FSTP_KB_GetActionName(Int32(aid))),
                group: String(cString: FSTP_KB_GetActionGroup(Int32(aid))),
                editable: FSTP_KB_IsActionEditable(Int32(aid)) != 0,
                keycode: Int(FSTP_KB_GetKeycode(Int32(aid))),
                mods: Int(FSTP_KB_GetMods(Int32(aid)))
            ))
        }
        items = newItems
    }

    var groups: [String] {
        var seen: [String] = []
        for it in items where !seen.contains(it.group) { seen.append(it.group) }
        return seen
    }

    func items(in group: String) -> [ActionItem] { items.filter { $0.group == group } }

    // Mac-style combo label, e.g. "⌃⇧K" or "Space".
    func comboString(keycode: Int, mods: Int) -> String {
        var s = ""
        if mods & 2 != 0 { s += "⌃" }   // Control
        if mods & 4 != 0 { s += "⌥" }   // Option
        if mods & 1 != 0 { s += "⇧" }   // Shift
        let nm = String(cString: FSTP_KB_GetKeyName(Int32(keycode)))
        s += nm.isEmpty ? "—" : nm
        return s
    }

    // MARK: live key capture
    func startCapture(_ action: Int) {
        stopCapture()
        capturingAction = action
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown]) { [weak self] event in
            guard let self = self else { return event }
            // Plain Escape cancels capture.
            if event.keyCode == 53
                && !event.modifierFlags.contains(.shift)
                && !event.modifierFlags.contains(.control)
                && !event.modifierFlags.contains(.option) {
                self.stopCapture()
                return nil
            }
            // ⌘ is reserved for menu shortcuts — abort rather than bind the bare key.
            if event.modifierFlags.contains(.command) {
                self.stopCapture()
                return nil
            }
            let chars = event.charactersIgnoringModifiers ?? ""
            let uni = chars.unicodeScalars.first.map { Int($0.value) } ?? 0
            let code = Int(FSTP_KB_MacKeyToSDL(Int32(event.keyCode), Int32(uni)))
            var mods = 0
            if event.modifierFlags.contains(.shift)   { mods |= 1 }
            if event.modifierFlags.contains(.control) { mods |= 2 }
            if event.modifierFlags.contains(.option)  { mods |= 4 }
            if code != 0 {
                self.assign(action: action, keycode: code, mods: mods)
            }
            self.stopCapture()
            return nil  // swallow the captured key
        }
    }

    func stopCapture() {
        if let m = monitor { NSEvent.removeMonitor(m); monitor = nil }
        capturingAction = nil
    }

    private func assign(action: Int, keycode: Int, mods: Int) {
        var conflict: Int32 = 0
        let r = FSTP_KB_SetBinding(Int32(action), Int32(keycode), Int32(mods), &conflict)
        if r == 0 {
            FSTP_KB_Save()
        } else if r == -1 {
            let other = String(cString: FSTP_KB_GetActionName(conflict))
            conflictMessage = "“\(comboString(keycode: keycode, mods: mods))” is already used by “\(other)”."
        }
        reload()
    }

    func resetDefaults() {
        FSTP_KB_ResetToDefaults()
        FSTP_KB_Save()
        reload()
    }
}

@available(macOS 13.0, *)
struct KeyboardSettingsView: View {
    @StateObject private var model = KeyboardSettingsModel()

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            VStack(alignment: .leading, spacing: 4) {
                Text("Keyboard Shortcuts").font(.headline)
                Text("Click a shortcut, then press the key (optionally with ⇧ ⌃ ⌥). Esc cancels. ⌘ and the modifier combos ⇧P, ⌥←/→, ⌘G, Shift+Backspace stay fixed.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            .padding(.horizontal, 20).padding(.top, 18).padding(.bottom, 10)

            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    ForEach(model.groups, id: \.self) { group in
                        VStack(alignment: .leading, spacing: 6) {
                            Text(group.uppercased())
                                .font(.system(size: 11, weight: .semibold))
                                .foregroundStyle(.secondary)

                            ForEach(model.items(in: group)) { item in
                                HStack {
                                    Text(item.name)
                                    Spacer()
                                    if item.editable {
                                        Button {
                                            if model.capturingAction == item.id {
                                                model.stopCapture()
                                            } else {
                                                model.startCapture(item.id)
                                            }
                                        } label: {
                                            Text(model.capturingAction == item.id
                                                 ? "Press a key…  (Esc)"
                                                 : model.comboString(keycode: item.keycode, mods: item.mods))
                                                .font(.system(.body, design: .monospaced))
                                                .frame(minWidth: 150)
                                        }
                                        .buttonStyle(.bordered)
                                        .tint(model.capturingAction == item.id ? Color.accentColor : nil)
                                    } else {
                                        Text(model.comboString(keycode: item.keycode, mods: item.mods))
                                            .font(.system(.body, design: .monospaced))
                                            .foregroundStyle(.secondary)
                                        Text("Fixed")
                                            .font(.caption2)
                                            .padding(.horizontal, 6).padding(.vertical, 2)
                                            .background(Color.secondary.opacity(0.15))
                                            .clipShape(Capsule())
                                    }
                                }
                                .padding(.vertical, 1)
                            }
                        }
                    }
                }
                .padding(.horizontal, 20).padding(.bottom, 16)
            }

            Divider()
            HStack {
                Button("Reset Shortcuts to Defaults") { model.resetDefaults() }
                Spacer()
            }
            .padding(.horizontal, 20).padding(.vertical, 12)
        }
        .onDisappear { model.stopCapture() }
        .alert("Shortcut in use", isPresented: Binding(
            get: { model.conflictMessage != nil },
            set: { if !$0 { model.conflictMessage = nil } }
        )) {
            Button("OK", role: .cancel) { model.conflictMessage = nil }
        } message: {
            Text(model.conflictMessage ?? "")
        }
    }
}
