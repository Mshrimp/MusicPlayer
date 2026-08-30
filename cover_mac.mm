// 封面解码兜底：Qt 官方 mac 包未带 webp 等图片插件时 QImage::fromData 会失败，
// 改用 macOS ImageIO 解码（macOS 11+ 原生支持 webp）。
#include <QByteArray>
#include <QImage>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

QImage imageFromMac(const QByteArray &data) {
    CFDataRef cfData = CFDataCreateWithBytesNoCopy(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(data.constData()),
        CFIndex(data.size()), kCFAllocatorNull);
    if (!cfData)
        return QImage();
    CGImageSourceRef src = CGImageSourceCreateWithData(cfData, nullptr);
    CFRelease(cfData);
    if (!src)
        return QImage();
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img)
        return QImage();

    const size_t w = CGImageGetWidth(img);
    const size_t h = CGImageGetHeight(img);
    QImage out(int(w), int(h), QImage::Format_RGBA8888);
    if (out.isNull()) {
        CGImageRelease(img);
        return QImage();
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        out.bits(), w, h, 8, size_t(out.bytesPerLine()), cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) {
        CGImageRelease(img);
        return QImage();
    }
    // CGContext 默认原点在左下，翻转后逐行写入才与 Qt「顶行在前」一致
    CGContextTranslateCTM(ctx, 0, CGFloat(h));
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, 0, CGFloat(w), CGFloat(h)), img);
    CGContextRelease(ctx);
    CGImageRelease(img);
    return out;
}
