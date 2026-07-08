import SwiftUI
import AppKit
import UniformTypeIdentifiers

// SwiftUI Memory Locations window — same modern look as the New/Edit dialog.
// Shows the focused player's markers (the C API is player-scoped); the ObjC side
// re-routes ShowMemoryLocations / refresh / is-active here.

@available(macOS 13.0, *)
struct MemoryLocationRow: Identifiable {
    let id: Int          // marker id == recall number
    let timecode: String
    let name: String
    let comments: String
}

@available(macOS 13.0, *)
final class MemoryLocationsListModel: ObservableObject {
    static let shared = MemoryLocationsListModel()

    @Published var rows: [MemoryLocationRow] = []
    @Published var selection: Int? = nil
    @Published var playerLabel: Int = 1

    func reload() {
        var newRows: [MemoryLocationRow] = []
        let count = Int(FSTP_GetMemoryLocationsCount())
        var data = FSTP_MemoryLocationData()
        for i in 0..<count {
            if FSTP_GetMemoryLocationData(Int32(i), &data) {
                let name = withUnsafeBytes(of: data.name) {
                    String(cString: $0.bindMemory(to: CChar.self).baseAddress!)
                }
                let comments = withUnsafeBytes(of: data.comments) {
                    String(cString: $0.bindMemory(to: CChar.self).baseAddress!)
                }
                let tc = withUnsafeBytes(of: data.timecode_display) {
                    String(cString: $0.bindMemory(to: CChar.self).baseAddress!)
                }
                newRows.append(MemoryLocationRow(id: Int(data.id), timecode: tc,
                                                 name: name, comments: comments))
            }
        }
        rows = newRows
        playerLabel = Int(GetActivePlayerID()) + 1
        if let sel = selection, !newRows.contains(where: { $0.id == sel }) {
            selection = nil
        }
    }
}

@available(macOS 13.0, *)
struct MemoryLocationsListView: View {
    @ObservedObject var model = MemoryLocationsListModel.shared

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            tableArea
            Divider()
            footer
        }
        .frame(minWidth: 540, minHeight: 320)
    }

    // MARK: Header
    private var header: some View {
        HStack(spacing: 12) {
            ZStack {
                Circle().fill(Color.accentColor.opacity(0.15)).frame(width: 34, height: 34)
                Image(systemName: "mappin.and.ellipse")
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundColor(.accentColor)
            }
            VStack(alignment: .leading, spacing: 1) {
                Text("Memory Locations").font(.system(size: 15, weight: .semibold))
                Text("Player \(model.playerLabel)  ·  \(model.rows.count) "
                     + (model.rows.count == 1 ? "marker" : "markers"))
                    .font(.system(size: 11)).foregroundStyle(.secondary)
            }
            Spacer()
        }
        .padding(.horizontal, 18)
        .padding(.vertical, 12)
        .background(.regularMaterial)
    }

    // MARK: Table
    private var tableArea: some View {
        Table(model.rows, selection: $model.selection) {
            TableColumn("#") { row in
                Text("\(row.id)").foregroundStyle(.secondary)
            }
            .width(min: 30, ideal: 36, max: 48)

            TableColumn("Timecode") { row in
                Text(row.timecode).font(.system(.body, design: .monospaced))
            }
            .width(min: 96, ideal: 116, max: 150)

            TableColumn("Name") { row in
                Text(row.name.isEmpty ? "—" : row.name)
                    .foregroundStyle(row.name.isEmpty ? .secondary : .primary)
            }
            .width(min: 120, ideal: 190)

            TableColumn("Comments") { row in
                Text(row.comments).foregroundStyle(.secondary)
            }
        }
        .contextMenu(forSelectionType: Int.self) { ids in
            if let id = ids.first {
                Button("Go to") { recall(id) }
                Button("Edit…") { edit(id) }
                Divider()
                Button("Delete", role: .destructive) { delete(id) }
            }
        } primaryAction: { ids in
            if let id = ids.first { recall(id) }
        }
        .tableStyle(.inset)
        .overlay {
            if model.rows.isEmpty {
                VStack(spacing: 6) {
                    Image(systemName: "mappin.slash")
                        .font(.system(size: 26))
                        .foregroundStyle(.tertiary)
                    Text("No memory locations")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                    Text("Press Enter during playback to add one")
                        .font(.system(size: 11))
                        .foregroundStyle(.tertiary)
                }
            }
        }
    }

    // MARK: Footer
    private var footer: some View {
        HStack(spacing: 8) {
            Button { addNew() } label: {
                Image(systemName: "plus")
            }
            .help("Add Memory Location")

            Button { if let id = model.selection { delete(id) } } label: {
                Image(systemName: "minus")
            }
            .help("Delete selected")
            .disabled(model.selection == nil)

            Spacer()

            Button("Import…") { importFile() }
                .help("Import Memory Locations from a file")
            Button("Export…") { exportCSV() }
                .help("Export Memory Locations to CSV")
        }
        .buttonStyle(.bordered)
        .controlSize(.large)
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(.regularMaterial)
    }

    // MARK: Actions (all operate on the focused player)
    private func recall(_ id: Int) {
        let player = GetActivePlayerID()
        if player >= 0 { _ = FSTP_RecallMemoryLocation(Int32(id), player) }
    }

    private func edit(_ id: Int) {
        let player = GetActivePlayerID()
        if player >= 0 {
            ShowSwiftUIMemoryLocationDialogEdit(player, GetInstancePosition(player), Int32(id))
        }
    }

    private func delete(_ id: Int) {
        _ = FSTP_DeleteMemoryLocation(Int32(id))
        model.reload()
    }

    private func addNew() {
        CreateMemoryLocationAtCurrentTime()
    }

    private func importFile() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.message = "Select Memory Locations file to import"
        if panel.runModal() == .OK, let url = panel.url {
            _ = FSTP_LoadMemoryLocations(url.path)
            model.reload()
        }
    }

    private func exportCSV() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "memory_locations.csv"
        panel.message = "Export Memory Locations to CSV"
        panel.allowedContentTypes = [.commaSeparatedText]
        if panel.runModal() == .OK, let url = panel.url {
            _ = FSTP_ExportMemoryLocationsToCSV(url.path)
        }
    }
}

// MARK: - Window lifecycle (Swift-owned, like the dialog)
@available(macOS 13.0, *)
final class MemoryLocationsWindowController: NSObject, NSWindowDelegate {
    static let shared = MemoryLocationsWindowController()
    var window: NSWindow?

    func show() {
        if window == nil {
            let hosting = NSHostingController(rootView: MemoryLocationsListView())
            let w = NSWindow(contentViewController: hosting)
            w.title = "Memory Locations"
            w.styleMask = [.titled, .closable, .resizable, .miniaturizable]
            w.setContentSize(NSSize(width: 620, height: 420))
            w.setFrameAutosaveName("MemoryLocationsWindow")
            w.isReleasedWhenClosed = false
            w.collectionBehavior = [.fullScreenNone]
            w.delegate = self
            w.center()
            window = w
        }
        MemoryLocationsListModel.shared.reload()
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func windowWillClose(_ notification: Notification) {
        window = nil
    }
}

// MARK: - C bridge
@_cdecl("ShowSwiftUIMemoryLocationsWindow")
public func ShowSwiftUIMemoryLocationsWindow() {
    if #available(macOS 13.0, *) {
        DispatchQueue.main.async { MemoryLocationsWindowController.shared.show() }
    }
}

@_cdecl("RefreshSwiftUIMemoryLocationsWindow")
public func RefreshSwiftUIMemoryLocationsWindow() {
    if #available(macOS 13.0, *) {
        DispatchQueue.main.async {
            if MemoryLocationsWindowController.shared.window != nil {
                MemoryLocationsListModel.shared.reload()
            }
        }
    }
}

@_cdecl("IsSwiftUIMemoryLocationsWindowOpen")
public func IsSwiftUIMemoryLocationsWindowOpen() -> Int32 {
    if #available(macOS 13.0, *) {
        return MemoryLocationsWindowController.shared.window != nil ? 1 : 0
    }
    return 0
}
