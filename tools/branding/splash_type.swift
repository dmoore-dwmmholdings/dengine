import Foundation
import CoreText
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers
let fontURL = URL(fileURLWithPath: CommandLine.arguments[1])
let out = CommandLine.arguments[2]
let descriptors = CTFontManagerCreateFontDescriptorsFromURL(fontURL as CFURL) as! [CTFontDescriptor]
func pathFor(_ text: String, _ name: String, _ tracking: CGFloat = 0) -> CGPath {
    let descriptor = descriptors.first { String(CTFontCopyPostScriptName(CTFontCreateWithFontDescriptor($0,100,nil))) == name }!
    let font = CTFontCreateWithFontDescriptor(descriptor,100,nil)
    let line = CTLineCreateWithAttributedString(NSAttributedString(string:text,attributes:[NSAttributedString.Key(kCTFontAttributeName as String):font,NSAttributedString.Key(kCTKernAttributeName as String):tracking]))
    let path = CGMutablePath()
    for run in CTLineGetGlyphRuns(line) as! [CTRun] {
        let count=CTRunGetGlyphCount(run)
        var glyphs=[CGGlyph](repeating:0,count:count),positions=[CGPoint](repeating:.zero,count:count)
        CTRunGetGlyphs(run,CFRange(location:0,length:0),&glyphs)
        CTRunGetPositions(run,CFRange(location:0,length:0),&positions)
        for i in 0..<count {
            if let glyph=CTFontCreatePathForGlyph(font,glyphs[i],nil) {
                path.addPath(glyph,transform:CGAffineTransform(translationX:positions[i].x,y:positions[i].y))
            }
        }
    }
    return path
}
func export(_ name:String,_ text:String,_ font:String,_ width:Int,_ height:Int,_ fitW:CGFloat,_ fitH:CGFloat,_ tracking:CGFloat=0) throws {
 let path=pathFor(text,font,tracking), bounds=path.boundingBoxOfPath
 let ctx=CGContext(data:nil,width:width,height:height,bitsPerComponent:8,bytesPerRow:width*4,space:CGColorSpaceCreateDeviceRGB(),bitmapInfo:CGImageAlphaInfo.premultipliedLast.rawValue)!
 ctx.setFillColor(CGColor(gray:0,alpha:1));ctx.fill(CGRect(x:0,y:0,width:width,height:height))
 let scale=min(fitW/bounds.width,fitH/bounds.height)
 ctx.translateBy(x:CGFloat(width)/2,y:CGFloat(height)/2);ctx.scaleBy(x:scale,y:scale);ctx.translateBy(x:-bounds.midX,y:-bounds.midY)
 ctx.setFillColor(CGColor(gray:1,alpha:1));ctx.addPath(path);ctx.fillPath()
 let destination=CGImageDestinationCreateWithURL(URL(fileURLWithPath:out+"/"+name+".png") as CFURL,UTType.png.identifier as CFString,1,nil)!
 CGImageDestinationAddImage(destination,ctx.makeImage()!,nil);CGImageDestinationFinalize(destination)
}
try export("emblem","D","Onest-ExtraBold",512,512,360,360)
try export("wordmark","DEngine","Onest-SemiBold",512,128,420,96)
try export("caption","POWERED BY","Onest-Medium",512,128,310,40,9)
