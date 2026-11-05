#include "remote/PairingQrCode.h"

#import <CoreImage/CoreImage.h>

#include <QByteArray>

#include <cmath>

namespace ketplus {

QImage makePairingQrCode(const QString& value, const int pixels) {
    if (value.isEmpty() || pixels < 64) return {};
    @autoreleasepool {
        const QByteArray utf8 = value.toUtf8();
        NSData* data = [NSData dataWithBytes:utf8.constData()
                                      length:static_cast<NSUInteger>(utf8.size())];
        CIFilter* filter = [CIFilter filterWithName:@"CIQRCodeGenerator"];
        [filter setValue:data forKey:@"inputMessage"];
        [filter setValue:@"M" forKey:@"inputCorrectionLevel"];
        CIImage* output = filter.outputImage;
        if (output == nil) return {};
        const CGRect extent = output.extent;
        const CGFloat scale = qMax(1.0, std::floor(pixels / CGRectGetWidth(extent)));
        CIImage* scaled = [output imageByApplyingTransform:CGAffineTransformMakeScale(scale, scale)];
        CIContext* context = [CIContext contextWithOptions:@{kCIContextUseSoftwareRenderer : @NO}];
        CGImageRef imageRef = [context createCGImage:scaled fromRect:scaled.extent];
        if (imageRef == nullptr) return {};
        const int width = static_cast<int>(CGImageGetWidth(imageRef));
        const int height = static_cast<int>(CGImageGetHeight(imageRef));
        QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        CGContextRef bitmap = CGBitmapContextCreate(
            image.bits(), width, height, 8, image.bytesPerLine(), colorSpace,
            kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);
        CGColorSpaceRelease(colorSpace);
        if (bitmap == nullptr) {
            CGImageRelease(imageRef);
            return {};
        }
        CGContextSetInterpolationQuality(bitmap, kCGInterpolationNone);
        CGContextTranslateCTM(bitmap, 0, height);
        CGContextScaleCTM(bitmap, 1, -1);
        CGContextDrawImage(bitmap, CGRectMake(0, 0, width, height), imageRef);
        CGContextRelease(bitmap);
        CGImageRelease(imageRef);
        return image;
    }
}

} // namespace ketplus
