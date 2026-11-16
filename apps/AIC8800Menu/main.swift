import Cocoa
import Darwin

private let kSockPath = "/var/run/aic8800_wifi.sock"
private let kLoginAgentLabel = "com.rush.aic8800_menu"

struct WifiStatus: Decodable {
    let ok: Bool
    let present: Bool
    let bsd: String
    let mac: String
    let fw: String
    let chip_id: String
    let lmac_version: String
    let is_5g: Bool
    let lmac_ready: Bool
    let associated: Bool
    let link_up: Bool
    let assoc_status: UInt32
    let disc_reason: UInt32
    let ssid: String
    let bssid: String
    let freq: UInt32
    let sta_idx: UInt32
    let tx_packets: UInt64
    let rx_packets: UInt64
    let eapol_rx_total: UInt64
    let eapol_rx_queued: UInt32
    let connecting: Bool
    let auto_paused: Bool
    let saved_ssid: String
    let last_error: String
}

struct ScanBss: Decodable {
    let ssid: String
    let hidden: Bool
    let bssid: String
    let freq: UInt32
    let rssi: Int
    let sec: String
    let open: Bool
}

struct ScanResponse: Decodable {
    let ok: Bool
    let scan_done: Bool?
    let bss: [ScanBss]?
    let error: String?
}

struct BasicResponse: Decodable {
    let ok: Bool
    let error: String?
}

private func hexEncode(_ s: String) -> String {
    return Data(s.utf8).map { String(format: "%02x", $0) }.joined()
}

private func ipv4AddressForInterface(_ bsdName: String) -> String? {
    guard !bsdName.isEmpty && bsdName != "unknown" else { return nil }
    var ifaddrPtr: UnsafeMutablePointer<ifaddrs>?
    guard getifaddrs(&ifaddrPtr) == 0, let first = ifaddrPtr else { return nil }
    defer { freeifaddrs(first) }

    var cur: UnsafeMutablePointer<ifaddrs>? = first
    while let node = cur {
        let iface = node.pointee
        if let namePtr = iface.ifa_name,
           let addrPtr = iface.ifa_addr,
           addrPtr.pointee.sa_family == UInt8(AF_INET) {
            let name = String(cString: namePtr)
            if name == bsdName {
                var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                let rc = getnameinfo(addrPtr,
                                     socklen_t(addrPtr.pointee.sa_len),
                                     &host,
                                     socklen_t(host.count),
                                     nil,
                                     0,
                                     NI_NUMERICHOST)
                if rc == 0 {
                    return String(cString: host)
                }
            }
        }
        cur = iface.ifa_next
    }
    return nil
}

private func sendDaemonCommand(_ cmd: String, timeoutSec: Int = 5) -> Result<Data, Error> {
    let fd = socket(AF_UNIX, SOCK_STREAM, 0)
    if fd < 0 {
        return .failure(NSError(domain: NSPOSIXErrorDomain, code: Int(errno)))
    }
    defer { close(fd) }

    var tv = timeval(tv_sec: timeoutSec, tv_usec: 0)
    _ = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
    _ = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

    var addr = sockaddr_un()
    addr.sun_family = sa_family_t(AF_UNIX)
    let pathBytes = Array(kSockPath.utf8CString)
    let maxLen = MemoryLayout.size(ofValue: addr.sun_path)
    if pathBytes.count > maxLen {
        return .failure(NSError(domain: NSPOSIXErrorDomain, code: Int(ENAMETOOLONG)))
    }
    withUnsafeMutableBytes(of: &addr.sun_path) { raw in
        for i in 0..<pathBytes.count {
            raw[i] = UInt8(bitPattern: pathBytes[i])
        }
    }

    let addrLen = socklen_t(MemoryLayout<sa_family_t>.size + pathBytes.count)
    let rc = withUnsafePointer(to: &addr) { ptr in
        ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
            Darwin.connect(fd, sa, addrLen)
        }
    }
    if rc != 0 {
        return .failure(NSError(domain: NSPOSIXErrorDomain, code: Int(errno)))
    }

    let payload = Array((cmd + "\n").utf8)
    var sent = 0
    while sent < payload.count {
        let n = payload.withUnsafeBytes { buf in
            Darwin.write(fd, buf.baseAddress!.advanced(by: sent), payload.count - sent)
        }
        if n <= 0 {
            return .failure(NSError(domain: NSPOSIXErrorDomain, code: Int(errno)))
        }
        sent += n
    }

    var reply = Data()
    var chunk = [UInt8](repeating: 0, count: 4096)
    while true {
        let n = Darwin.read(fd, &chunk, chunk.count)
        if n < 0 {
            if errno == EINTR { continue }
            return .failure(NSError(domain: NSPOSIXErrorDomain, code: Int(errno)))
        }
        if n == 0 {
            break
        }
        reply.append(contentsOf: chunk[0..<n])
        if chunk[0..<n].contains(0x0a) {
            break
        }
    }
    return .success(reply)
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var statusItem: NSStatusItem!
    private let menu = NSMenu()
    private let ioQueue = DispatchQueue(label: "com.rush.AIC8800Menu.io", qos: .userInitiated)

    private var pollTimer: Timer?
    private var currentStatus: WifiStatus?
    private var currentIPv4: String?
    private var daemonOffline = true
    private var isScanning = false
    private var cachedNetworks: [ScanBss] = []
    private var actionMessage: String?
    private var pendingAutoScan = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        menu.delegate = self
        menu.autoenablesItems = false
        statusItem.menu = menu

        updateIconAndTitle()
        rebuildMenu()

        pollStatus()
        pollTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            self?.pollStatus()
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        pollTimer?.invalidate()
        pollTimer = nil
    }

    func menuWillOpen(_ menu: NSMenu) {
        pollStatus()
        if !daemonOffline, let st = currentStatus, st.present, st.lmac_ready, !isScanning {
            triggerScan()
        }
        rebuildMenu()
    }

    private func pollStatus() {
        ioQueue.async { [weak self] in
            guard let self = self else { return }
            switch sendDaemonCommand("STATUS", timeoutSec: 3) {
            case .success(let data):
                if let st = try? JSONDecoder().decode(WifiStatus.self, from: data), st.ok {
                    let ip = (st.associated && st.link_up) ? ipv4AddressForInterface(st.bsd) : nil
                    DispatchQueue.main.async {
                        self.daemonOffline = false
                        self.currentStatus = st
                        self.currentIPv4 = ip
                        if !self.pendingAutoScan && st.present && st.lmac_ready && self.cachedNetworks.isEmpty {
                            self.pendingAutoScan = true
                            self.triggerScan()
                        }
                        self.updateIconAndTitle()
                        self.rebuildMenu()
                    }
                    return
                }
                DispatchQueue.main.async {
                    self.daemonOffline = true
                    self.currentStatus = nil
                    self.currentIPv4 = nil
                    self.updateIconAndTitle()
                    self.rebuildMenu()
                }
            case .failure:
                DispatchQueue.main.async {
                    self.daemonOffline = true
                    self.currentStatus = nil
                    self.currentIPv4 = nil
                    self.updateIconAndTitle()
                    self.rebuildMenu()
                }
            }
        }
    }

    private func triggerScan() {
        guard !isScanning else { return }
        isScanning = true
        rebuildMenu()

        ioQueue.async { [weak self] in
            guard let self = self else { return }
            let res = sendDaemonCommand("SCAN", timeoutSec: 15)
            DispatchQueue.main.async {
                self.isScanning = false
                switch res {
                case .success(let data):
                    if let resp = try? JSONDecoder().decode(ScanResponse.self, from: data),
                       resp.ok, let list = resp.bss {
                        self.cachedNetworks = self.deduplicateAndSort(list)
                        self.actionMessage = nil
                    } else if let resp = try? JSONDecoder().decode(ScanResponse.self, from: data),
                              let err = resp.error {
                        self.actionMessage = err
                    }
                case .failure(let err):
                    self.actionMessage = err.localizedDescription
                }
                self.rebuildMenu()
            }
        }
    }

    private func deduplicateAndSort(_ raw: [ScanBss]) -> [ScanBss] {
        var bestBySsid: [String: ScanBss] = [:]
        for item in raw where !item.hidden && !item.ssid.isEmpty {
            if let existing = bestBySsid[item.ssid] {
                if item.rssi > existing.rssi {
                    bestBySsid[item.ssid] = item
                }
            } else {
                bestBySsid[item.ssid] = item
            }
        }
        let activeSsid = (currentStatus?.associated == true) ? currentStatus?.ssid : nil
        return bestBySsid.values.sorted { a, b in
            if a.ssid == activeSsid && b.ssid != activeSsid { return true }
            if b.ssid == activeSsid && a.ssid != activeSsid { return false }
            return a.rssi > b.rssi
        }
    }

    private func sendSimpleAction(_ cmd: String, timeoutSec: Int = 10) {
        ioQueue.async { [weak self] in
            let res = sendDaemonCommand(cmd, timeoutSec: timeoutSec)
            DispatchQueue.main.async {
                switch res {
                case .success(let data):
                    if let resp = try? JSONDecoder().decode(BasicResponse.self, from: data), !resp.ok {
                        self?.actionMessage = resp.error ?? "Command failed"
                    } else {
                        self?.actionMessage = nil
                    }
                case .failure(let err):
                    self?.actionMessage = err.localizedDescription
                }
                self?.pollStatus()
            }
        }
    }

    private func performConnect(ssid: String, passphrase: String?, save: Bool) {
        let ssidHex = hexEncode(ssid)
        let passHex = (passphrase != nil && !passphrase!.isEmpty) ? hexEncode(passphrase!) : "-"
        let saveFlag = save ? 1 : 0
        actionMessage = nil
        updateIconAndTitle()
        rebuildMenu()

        ioQueue.async { [weak self] in
            let res = sendDaemonCommand("CONNECT_HEX \(ssidHex) \(passHex) \(saveFlag)", timeoutSec: 25)
            DispatchQueue.main.async {
                switch res {
                case .success(let data):
                    if let resp = try? JSONDecoder().decode(BasicResponse.self, from: data), !resp.ok {
                        self?.actionMessage = resp.error ?? "Connection failed"
                    } else {
                        self?.actionMessage = nil
                    }
                case .failure(let err):
                    self?.actionMessage = err.localizedDescription
                }
                self?.pollStatus()
            }
        }
    }

    private func updateIconAndTitle() {
        guard let button = statusItem.button else { return }
        let symbolName: String
        var title = ""

        if daemonOffline || currentStatus == nil || currentStatus?.present == false {
            symbolName = "wifi.slash"
        } else if let st = currentStatus, st.connecting {
            symbolName = "wifi"
            title = " …"
        } else if let st = currentStatus, st.associated && st.link_up {
            symbolName = "wifi"
        } else {
            symbolName = "wifi.exclamationmark"
        }

        if let img = NSImage(systemSymbolName: symbolName, accessibilityDescription: "AIC8800 Wi-Fi") {
            img.isTemplate = true
            button.image = img
        } else {
            button.image = nil
            title = "WiFi"
        }
        button.title = title
    }

    private func disabledInfoItem(_ text: String, bold: Bool = false) -> NSMenuItem {
        let item = NSMenuItem(title: text, action: nil, keyEquivalent: "")
        item.isEnabled = false
        if bold {
            let font = NSFont.menuFont(ofSize: 13)
            let boldFont = NSFontManager.shared.convert(font, toHaveTrait: .boldFontMask)
            item.attributedTitle = NSAttributedString(string: text, attributes: [.font: boldFont])
        }
        return item
    }

    private func rebuildMenu() {
        menu.removeAllItems()

        if daemonOffline {
            menu.addItem(disabledInfoItem("AIC8800 Daemon Offline", bold: true))
            menu.addItem(disabledInfoItem("Run: sudo make install-daemon"))
        } else if let st = currentStatus {
            if !st.present {
                menu.addItem(disabledInfoItem("AIC8800 Adapter Not Found", bold: true))
                menu.addItem(disabledInfoItem("Plug in AIC8800D80 USB adapter"))
            } else if !st.lmac_ready {
                menu.addItem(disabledInfoItem("AIC8800 Initializing Firmware… (\(st.bsd))", bold: true))
            } else if st.connecting {
                menu.addItem(disabledInfoItem("Connecting… (\(st.bsd))", bold: true))
            } else if st.associated && st.link_up {
                let band = st.freq >= 5000 ? "5 GHz" : "2.4 GHz"
                let ssidShow = st.ssid.isEmpty ? "(hidden)" : st.ssid
                menu.addItem(disabledInfoItem("Connected: \(ssidShow)", bold: true))
                let ipText = currentIPv4 ?? "DHCP pending…"
                menu.addItem(disabledInfoItem("  \(band) (\(st.freq) MHz) • \(st.bsd) • IP \(ipText)"))
                menu.addItem(disabledInfoItem("  BSSID \(st.bssid) • TX \(st.tx_packets) / RX \(st.rx_packets)"))
            } else {
                let extra = st.auto_paused ? " (paused)" : ""
                menu.addItem(disabledInfoItem("Disconnected\(extra) • \(st.bsd)", bold: true))
                if st.disc_reason != 0 {
                    menu.addItem(disabledInfoItem("  Last disconnect reason: \(st.disc_reason)"))
                }
            }

            let err = actionMessage ?? (st.last_error.isEmpty ? nil : st.last_error)
            if let e = err, !e.isEmpty {
                menu.addItem(disabledInfoItem("⚠ \(e)"))
            }

            if st.present && st.lmac_ready {
                menu.addItem(NSMenuItem.separator())

                if st.associated || st.connecting {
                    let discItem = NSMenuItem(title: "Disconnect", action: #selector(onDisconnect), keyEquivalent: "")
                    discItem.target = self
                    menu.addItem(discItem)
                } else if !st.saved_ssid.isEmpty {
                    let recItem = NSMenuItem(title: "Connect to Saved: \(st.saved_ssid)", action: #selector(onReconnect), keyEquivalent: "")
                    recItem.target = self
                    menu.addItem(recItem)
                }

                if !st.saved_ssid.isEmpty {
                    let forgetItem = NSMenuItem(title: "Forget \"\(st.saved_ssid)\"", action: #selector(onForget), keyEquivalent: "")
                    forgetItem.target = self
                    menu.addItem(forgetItem)
                }

                menu.addItem(NSMenuItem.separator())
                let scanTitle = isScanning ? "Scanning Networks…" : "Scan Networks"
                let scanItem = NSMenuItem(title: scanTitle, action: #selector(onScan), keyEquivalent: "r")
                scanItem.target = self
                scanItem.isEnabled = !isScanning
                menu.addItem(scanItem)

                if cachedNetworks.isEmpty && !isScanning {
                    menu.addItem(disabledInfoItem("  No networks scanned yet"))
                } else {
                    for bss in cachedNetworks {
                        let isCurrent = (st.associated && st.ssid == bss.ssid)
                        let band = bss.freq >= 5000 ? "5G" : "2.4G"
                        let title = "\(bss.ssid)  [\(band) • \(bss.rssi) dBm • \(bss.sec)]"
                        let netItem = NSMenuItem(title: title, action: #selector(onSelectNetwork(_:)), keyEquivalent: "")
                        netItem.target = self
                        netItem.representedObject = bss
                        netItem.state = isCurrent ? .on : .off
                        let sym = bss.open ? "wifi" : "lock.fill"
                        if let icon = NSImage(systemSymbolName: sym, accessibilityDescription: bss.sec) {
                            icon.isTemplate = true
                            netItem.image = icon
                        }
                        menu.addItem(netItem)
                    }
                }

                let hiddenItem = NSMenuItem(title: "Join Other / Hidden Network…", action: #selector(onJoinHidden), keyEquivalent: "")
                hiddenItem.target = self
                menu.addItem(hiddenItem)
            }
        }

        menu.addItem(NSMenuItem.separator())

        let loginItem = NSMenuItem(title: "Launch at Login", action: #selector(onToggleLogin), keyEquivalent: "")
        loginItem.target = self
        loginItem.state = isLoginAgentEnabled() ? .on : .off
        menu.addItem(loginItem)

        let logItem = NSMenuItem(title: "Open Wi-Fi Log", action: #selector(onOpenLog), keyEquivalent: "")
        logItem.target = self
        menu.addItem(logItem)

        let quitItem = NSMenuItem(title: "Quit AIC8800Menu", action: #selector(onQuit), keyEquivalent: "q")
        quitItem.target = self
        menu.addItem(quitItem)
    }

    @objc private func onScan() {
        triggerScan()
    }

    @objc private func onDisconnect() {
        sendSimpleAction("DISCONNECT", timeoutSec: 8)
    }

    @objc private func onReconnect() {
        sendSimpleAction("RECONNECT", timeoutSec: 8)
    }

    @objc private func onForget() {
        sendSimpleAction("FORGET", timeoutSec: 8)
    }

    @objc private func onSelectNetwork(_ sender: NSMenuItem) {
        guard let bss = sender.representedObject as? ScanBss else { return }
        if bss.open {
            performConnect(ssid: bss.ssid, passphrase: nil, save: true)
            return
        }
        promptForPassword(ssid: bss.ssid, secLabel: bss.sec, allowEditSsid: false)
    }

    @objc private func onJoinHidden() {
        promptForPassword(ssid: "", secLabel: "WPA2-PSK or Open", allowEditSsid: true)
    }

    private func promptForPassword(ssid: String, secLabel: String, allowEditSsid: Bool) {
        NSApp.activate(ignoringOtherApps: true)

        let alert = NSAlert()
        alert.alertStyle = .informational
        alert.messageText = allowEditSsid
            ? "Join Wi-Fi Network"
            : "Connect to \"\(ssid)\""
        alert.informativeText = allowEditSsid
            ? "Enter the network SSID and optional WPA2 passphrase (leave password blank for Open):"
            : "Security: \(secLabel)"
        alert.addButton(withTitle: "Join")
        alert.addButton(withTitle: "Cancel")

        let stackHeight: CGFloat = allowEditSsid ? 92 : 60
        let container = NSView(frame: NSRect(x: 0, y: 0, width: 290, height: stackHeight))

        var y: CGFloat = stackHeight - 24
        var ssidField: NSTextField?
        if allowEditSsid {
            let f = NSTextField(frame: NSRect(x: 0, y: y, width: 290, height: 24))
            f.placeholderString = "Network SSID"
            f.stringValue = ssid
            container.addSubview(f)
            ssidField = f
            y -= 32
        }

        let secField = NSSecureTextField(frame: NSRect(x: 0, y: y, width: 290, height: 24))
        secField.placeholderString = allowEditSsid ? "Passphrase (8..63 chars, blank if open)" : "Password"
        container.addSubview(secField)
        y -= 28

        let rememberCheck = NSButton(checkboxWithTitle: "Remember this network", target: nil, action: nil)
        rememberCheck.frame = NSRect(x: 0, y: y, width: 290, height: 20)
        rememberCheck.state = .on
        container.addSubview(rememberCheck)

        alert.accessoryView = container
        alert.window.initialFirstResponder = ssidField ?? secField

        let response = alert.runModal()
        guard response == .alertFirstButtonReturn else { return }

        let chosenSsid = (ssidField?.stringValue ?? ssid).trimmingCharacters(in: .whitespacesAndNewlines)
        let pass = secField.stringValue
        guard !chosenSsid.isEmpty else { return }

        if !allowEditSsid && pass.count < 8 {
            actionMessage = "Passphrase must be at least 8 characters"
            rebuildMenu()
            return
        }
        if !pass.isEmpty && (pass.count < 8 || pass.count > 63) {
            actionMessage = "Passphrase must be 8..63 characters"
            rebuildMenu()
            return
        }

        performConnect(ssid: chosenSsid,
                       passphrase: pass.isEmpty ? nil : pass,
                       save: rememberCheck.state == .on)
    }

    private func loginAgentURL() -> URL {
        let home = FileManager.default.homeDirectoryForCurrentUser
        return home.appendingPathComponent("Library/LaunchAgents/\(kLoginAgentLabel).plist")
    }

    private func isLoginAgentEnabled() -> Bool {
        return FileManager.default.fileExists(atPath: loginAgentURL().path)
    }

    @objc private func onToggleLogin() {
        let url = loginAgentURL()
        let fm = FileManager.default
        if fm.fileExists(atPath: url.path) {
            try? fm.removeItem(at: url)
            rebuildMenu()
            return
        }

        let exePath = Bundle.main.executablePath ?? "/Applications/AIC8800Menu.app/Contents/MacOS/AIC8800Menu"
        let plist: [String: Any] = [
            "Label": kLoginAgentLabel,
            "ProgramArguments": [exePath],
            "RunAtLoad": true,
            "KeepAlive": false
        ]
        try? fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        if let data = try? PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0) {
            try? data.write(to: url)
        }
        rebuildMenu()
    }

    @objc private func onOpenLog() {
        let url = URL(fileURLWithPath: "/var/log/aic8800_wifi.log")
        NSWorkspace.shared.open(url)
    }

    @objc private func onQuit() {
        NSApplication.shared.terminate(nil)
    }
}

@main
enum AIC8800MenuMain {
    static func main() {
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        let delegate = AppDelegate()
        app.delegate = delegate
        app.run()
    }
}
