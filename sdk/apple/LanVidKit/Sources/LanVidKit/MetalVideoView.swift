import CoreImage
import CoreVideo
import Metal
import MetalKit
import SwiftUI

/// Zero-copy Metal renderer for `CVPixelBuffer` streams on both macOS and iOS (`MTKView`).
public final class LanVidMetalRenderer: NSObject, MTKViewDelegate {
    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private let ciContext: CIContext
    private let lock = NSLock()
    private var latestPixelBuffer: CVPixelBuffer?

    public init?(metalView: MTKView) {
        guard let dev = MTLCreateSystemDefaultDevice(),
              let queue = dev.makeCommandQueue() else { return nil }
        self.device = dev
        self.commandQueue = queue
        self.ciContext = CIContext(mtlDevice: dev)
        super.init()
        metalView.device = dev
        metalView.framebufferOnly = false
        metalView.isPaused = true
        metalView.enableSetNeedsDisplay = true
        metalView.delegate = self
    }

    public func enqueue(_ pixelBuffer: CVPixelBuffer, in view: MTKView) {
        lock.lock()
        latestPixelBuffer = pixelBuffer
        lock.unlock()
        DispatchQueue.main.async {
            view.setNeedsDisplay(view.bounds)
        }
    }

    public func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    public func draw(in view: MTKView) {
        lock.lock()
        let pb = latestPixelBuffer
        lock.unlock()
        guard let pb,
              let drawable = view.currentDrawable,
              let cmdBuf = commandQueue.makeCommandBuffer() else { return }

        let image = CIImage(cvPixelBuffer: pb)
        let dSize = view.drawableSize
        let sx = dSize.width / image.extent.width
        let sy = dSize.height / image.extent.height
        let scale = min(sx, sy)
        let scaled = image.transformed(by: CGAffineTransform(scaleX: scale, y: scale))
        let tx = (dSize.width - scaled.extent.width) * 0.5
        let ty = (dSize.height - scaled.extent.height) * 0.5
        let centered = scaled.transformed(by: CGAffineTransform(translationX: tx, y: ty))

        ciContext.render(
            centered,
            to: drawable.texture,
            commandBuffer: cmdBuf,
            bounds: CGRect(origin: .zero, size: dSize),
            colorSpace: CGColorSpaceCreateDeviceRGB()
        )
        cmdBuf.present(drawable)
        cmdBuf.commit()
    }
}
