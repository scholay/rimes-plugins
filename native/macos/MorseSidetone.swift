import AVFoundation
import Foundation

/// The Morse sound: a sine sidetone while Space is held, a short high blip
/// when a letter is decoded and a low one for a code that means nothing.
/// Attack and release ramps keep the tone free of clicks.
struct MorseSynthKernel {
    static let toneFrequency: Double = 680
    static let toneLevel: Float = 0.2
    static let rampSeconds: Double = 0.005

    let sampleRate: Double
    private var tonePhase: Double = 0
    private var gain: Float = 0
    private var blipPhase: Double = 0
    private var blipFrequency: Double = 0
    private var blipRemaining = 0
    private var blipTotal = 0

    /// Written by the main thread, read by the render thread; single aligned
    /// word stores, so a render cycle sees either the old or the new value.
    var keyed = false

    init(sampleRate: Double) { self.sampleRate = sampleRate }

    mutating func blip(frequency: Double, seconds: Double) {
        blipFrequency = frequency
        blipTotal = max(1, Int(seconds * sampleRate))
        blipRemaining = blipTotal
        blipPhase = 0
    }

    mutating func render(into samples: UnsafeMutablePointer<Float>, count: Int) {
        let step = Float(1 / (Self.rampSeconds * sampleRate))
        let toneIncrement = 2 * Double.pi * Self.toneFrequency / sampleRate
        let blipIncrement = 2 * Double.pi * blipFrequency / sampleRate
        for index in 0..<count {
            gain = keyed ? min(1, gain + step) : max(0, gain - step)
            var value = Float(sin(tonePhase)) * gain * Self.toneLevel
            tonePhase += toneIncrement
            if tonePhase > 2 * Double.pi { tonePhase -= 2 * Double.pi }
            if blipRemaining > 0 {
                // A raised-cosine envelope: no click at either end.
                let progress = Double(blipTotal - blipRemaining) / Double(blipTotal)
                let envelope = Float(0.5 - 0.5 * cos(2 * Double.pi * progress))
                value += Float(sin(blipPhase)) * envelope * 0.12
                blipPhase += blipIncrement
                blipRemaining -= 1
            }
            samples[index] = value
        }
    }
}

/// Owns the audio engine. It starts only when Morse is selected and a key is
/// pressed, and shuts down after a quiet spell so the device is not held.
final class MorseSidetone {
    static let shared = MorseSidetone()
    static let idleShutdown: TimeInterval = 20

    private var engine: AVAudioEngine?
    private let kernel: UnsafeMutablePointer<MorseSynthKernel>
    private var idleTimer: Timer?

    init() {
        kernel = UnsafeMutablePointer<MorseSynthKernel>.allocate(capacity: 1)
        kernel.initialize(to: MorseSynthKernel(sampleRate: 48_000))
    }

    deinit {
        engine?.stop()
        kernel.deinitialize(count: 1)
        kernel.deallocate()
    }

    var isRunning: Bool { engine?.isRunning == true }

    func keyDown() {
        guard ensureRunning() else { return }
        kernel.pointee.keyed = true
        scheduleIdleShutdown()
    }

    func keyUp() {
        kernel.pointee.keyed = false
        scheduleIdleShutdown()
    }

    func letterBlip() {
        guard ensureRunning() else { return }
        kernel.pointee.blip(frequency: 1_320, seconds: 0.035)
        scheduleIdleShutdown()
    }

    func rejectBlip() {
        guard ensureRunning() else { return }
        kernel.pointee.blip(frequency: 220, seconds: 0.09)
        scheduleIdleShutdown()
    }

    func shutdown() {
        dispatchPrecondition(condition: .onQueue(.main))
        idleTimer?.invalidate()
        idleTimer = nil
        kernel.pointee.keyed = false
        engine?.stop()
        engine = nil
    }

    private func ensureRunning() -> Bool {
        dispatchPrecondition(condition: .onQueue(.main))
        if let engine, engine.isRunning { return true }
        let engine = AVAudioEngine()
        let output = engine.outputNode.inputFormat(forBus: 0)
        let sampleRate = output.sampleRate > 0 ? output.sampleRate : 48_000
        guard let format = AVAudioFormat(standardFormatWithSampleRate: sampleRate, channels: 1) else {
            return false
        }
        kernel.pointee = MorseSynthKernel(sampleRate: sampleRate)
        let kernel = self.kernel
        let source = AVAudioSourceNode(format: format) { _, _, frameCount, audioBufferList in
            let buffers = UnsafeMutableAudioBufferListPointer(audioBufferList)
            guard let data = buffers.first?.mData?.assumingMemoryBound(to: Float.self) else {
                return noErr
            }
            kernel.pointee.render(into: data, count: Int(frameCount))
            for buffer in buffers.dropFirst() {
                buffer.mData?.copyMemory(from: data, byteCount: Int(buffer.mDataByteSize))
            }
            return noErr
        }
        engine.attach(source)
        engine.connect(source, to: engine.mainMixerNode, format: format)
        do {
            try engine.start()
        } catch {
            IMELog.write("morse sidetone engine failed to start")
            return false
        }
        self.engine = engine
        return true
    }

    private func scheduleIdleShutdown() {
        idleTimer?.invalidate()
        let timer = Timer(timeInterval: Self.idleShutdown, repeats: false) { [weak self] _ in
            self?.shutdown()
        }
        idleTimer = timer
        RunLoop.main.add(timer, forMode: .common)
    }
}
