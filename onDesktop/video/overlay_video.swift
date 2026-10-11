import Foundation
import AVFoundation
import AppKit
import CoreImage
struct Row:Decodable {let video_s:Double;let elapsed:String;let state:String;let live:[String]}
struct Stat:Decodable {let label:String;let n:Int;let q1:Double;let median:Double;let q3:Double}
struct Overlay:Decodable {let rows:[Row];let stats:[Stat];let run_label:String;let duration_s:Double;let uncertainty_ms:Double;let note:String;let command_lines:[String]?;let rate_hz:Double?}
let args=CommandLine.arguments
let previewTime:Double? = args.count == 5 ? Double(args[4]) : nil
guard args.count==4 || previewTime != nil else{print("Usage: overlay_video raw.mp4 overlay.json output.mp4 [preview_seconds; output must be PNG]");exit(2)}
let data=try JSONDecoder().decode(Overlay.self,from:Data(contentsOf:URL(fileURLWithPath:args[2])))
let rows=data.rows
guard !rows.isEmpty else{exit(3)}
let asset=AVURLAsset(url:URL(fileURLWithPath:args[1]))
guard let track=asset.tracks(withMediaType:.video).first else{exit(4)}
let reader=try AVAssetReader(asset:asset)
let output=AVAssetReaderTrackOutput(track:track,outputSettings:[kCVPixelBufferPixelFormatTypeKey as String:kCVPixelFormatType_32BGRA]);output.alwaysCopiesSampleData=false;reader.add(output)
let writer=try AVAssetWriter(outputURL: previewTime == nil ? URL(fileURLWithPath:args[3]) : FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString+".mp4"),fileType:.mp4)
let w=Int(track.naturalSize.width),h=Int(track.naturalSize.height)
let input=AVAssetWriterInput(mediaType:.video,outputSettings:[AVVideoCodecKey:AVVideoCodecType.h264,AVVideoWidthKey:w,AVVideoHeightKey:h,AVVideoCompressionPropertiesKey:[AVVideoAverageBitRateKey:8_000_000]])
let adapter=AVAssetWriterInputPixelBufferAdaptor(assetWriterInput:input,sourcePixelBufferAttributes:[kCVPixelBufferPixelFormatTypeKey as String:kCVPixelFormatType_32BGRA,kCVPixelBufferWidthKey as String:w,kCVPixelBufferHeightKey as String:h,kCVPixelBufferCGImageCompatibilityKey as String:true,kCVPixelBufferCGBitmapContextCompatibilityKey as String:true])
writer.add(input);guard reader.startReading() else{exit(5)}
if previewTime == nil {guard writer.startWriting() else{exit(5)};writer.startSession(atSourceTime:.zero)}
let ci=CIContext();let color=CGColorSpaceCreateDeviceRGB()
var index=0,count=0,failed=false,truncated=false
let clipStart=rows.first(where:{$0.state != "WAITING"})!.video_s
let cutoff=(rows.first(where:{$0.state.hasPrefix("STOP")})?.video_s ?? rows.last!.video_s)+2
func label(_ text:String,_ x:CGFloat,_ y:CGFloat,_ size:CGFloat,_ color:NSColor = .white) {
 let attrs:[NSAttributedString.Key:Any]=[.font:NSFont.monospacedSystemFont(ofSize:size,weight:.medium),.foregroundColor:color]
 (text as NSString).draw(at:NSPoint(x:x,y:y),withAttributes:attrs)
}
while let sample=output.copyNextSampleBuffer() {
 if previewTime == nil && CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample))<clipStart {continue}
 if let t=previewTime, CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample)) < t {continue}
 if CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample))>cutoff {truncated=true;reader.cancelReading();break}
 autoreleasepool {
  let pts=CMSampleBufferGetPresentationTimeStamp(sample);let time=CMTimeGetSeconds(pts)
  while index+1<rows.count && rows[index+1].video_s<=time+0.0001{index+=1}
  let row=rows[index]
  guard let original=CMSampleBufferGetImageBuffer(sample) else{failed=true;return}
  while !input.isReadyForMoreMediaData && writer.status == .writing {usleep(1000)}
  guard previewTime != nil || writer.status == .writing else{failed=true;return}
  var result:CVPixelBuffer?
  let allocated:CVReturn
  if previewTime != nil {allocated=CVPixelBufferCreate(nil,w,h,kCVPixelFormatType_32BGRA,[kCVPixelBufferCGImageCompatibilityKey:true,kCVPixelBufferCGBitmapContextCompatibilityKey:true] as CFDictionary,&result)}
  else {allocated=CVPixelBufferPoolCreatePixelBuffer(nil,adapter.pixelBufferPool!,&result)}
  guard allocated==kCVReturnSuccess,let pixel=result else{failed=true;return}
  ci.render(CIImage(cvPixelBuffer:original),to:pixel)
  CVPixelBufferLockBaseAddress(pixel,[])
  let context=CGContext(data:CVPixelBufferGetBaseAddress(pixel),width:w,height:h,bitsPerComponent:8,bytesPerRow:CVPixelBufferGetBytesPerRow(pixel),space:color,bitmapInfo:CGImageAlphaInfo.premultipliedFirst.rawValue|CGBitmapInfo.byteOrder32Little.rawValue)!
  context.setFillColor(CGColor(red:0.015,green:0.025,blue:0.045,alpha:0.91))
  context.fill(CGRect(x:0,y:0,width:w,height:360))
  NSGraphicsContext.saveGraphicsState();NSGraphicsContext.current=NSGraphicsContext(cgContext:context,flipped:false)
  let cyan=NSColor(calibratedRed:0.4,green:0.85,blue:1,alpha:1)
  label("LIVE | \(data.run_label)",30,312,31,cyan)
  for (i,line) in row.live.enumerated(){label(line,30,CGFloat(265-i*40),28)}
  for (i,line) in (data.command_lines ?? ["CMD counts unavailable"]).enumerated(){label(line,30,CGFloat(50-i*35),25,cyan)}
  context.setStrokeColor(CGColor(gray:0.5,alpha:0.7));context.setLineWidth(1)
  context.move(to:CGPoint(x:920,y:15));context.addLine(to:CGPoint(x:920,y:343));context.strokePath()
  label("RUN TIMING (ms) | \(String(format:"%.0f",data.rate_hz ?? 100)) Hz | Duration \(String(format: "%.1f",data.duration_s)) s",960,312,30,cyan)
  label("Q1",1300,267,30);label("Median",1500,267,30);label("Q3",1740,267,30)
  for (i,s) in data.stats.enumerated(){
    let y=CGFloat(232-i*38)
    let c:NSColor = (s.label=="Round trip" || s.label=="Period") ? cyan : .white
    label(s.label,950,y,29,c)
    let f=s.label == "Desktop compute" ? "%7.3f" : "%7.1f"
    label(String(format:f,s.q1),1250,y,32,c)
    label(String(format:f,s.median),1480,y,32,c)
    label(String(format:f,s.q3),1710,y,32,c)
  }
  label(data.note,960,7,22,cyan)
  NSGraphicsContext.restoreGraphicsState()
  if previewTime != nil {
    guard let cg=context.makeImage(),let png=NSBitmapImageRep(cgImage:cg).representation(using:.png,properties:[:]) else{exit(7)}
    do {try png.write(to:URL(fileURLWithPath:args[3]))} catch {print(error);exit(7)}
    CVPixelBufferUnlockBaseAddress(pixel,[]);reader.cancelReading();print("Saved preview frame");exit(0)
  }
  CVPixelBufferUnlockBaseAddress(pixel,[])
  if !adapter.append(pixel,withPresentationTime:CMTimeSubtract(pts,CMTime(seconds:clipStart,preferredTimescale:1_000_000_000))){failed=true}
  count+=1
 }
 if failed{break}
}
input.markAsFinished();let done=DispatchSemaphore(value:0);writer.finishWriting{done.signal()};done.wait()
print("Rendered \(count) frames; reader=\(reader.status.rawValue) writer=\(writer.status.rawValue)")
if failed || (reader.status != .completed && !truncated) || writer.status != .completed {print(String(describing:writer.error));exit(6)}
