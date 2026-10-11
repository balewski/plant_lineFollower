import Foundation
import AVFoundation
import Darwin
func mono()->Double { var t=timespec();clock_gettime(CLOCK_MONOTONIC,&t);return Double(t.tv_sec)*1e9+Double(t.tv_nsec) }
let args=CommandLine.arguments
guard args.count==4,let duration=Double(args[2]),duration>0,duration<=120 else {
 print("usage: record_timed OUTPUT.mp4 SECONDS CAMERA_NAME"); exit(2)
}
let cameraName=args[3]
let cameraStatus=AVCaptureDevice.authorizationStatus(for:.video)
if cameraStatus == .notDetermined {
 let allow=DispatchSemaphore(value:0); var granted=false
 AVCaptureDevice.requestAccess(for:.video) { granted=$0; allow.signal() }
 allow.wait()
 if !granted { print("Camera access denied"); exit(3) }
} else if cameraStatus != .authorized {
 print("Camera access is not authorized (status \(cameraStatus.rawValue)). Allow Camera for this app in System Settings.")
 exit(3)
}
guard let device=AVCaptureDevice.devices(for:.video).first(where:{$0.localizedName==cameraName}) else {
 print("Camera not found: \(cameraName)"); exit(4)
}
let session=AVCaptureSession();session.sessionPreset = .hd1920x1080
session.addInput(try AVCaptureDeviceInput(device:device))
let output=AVCaptureVideoDataOutput();output.alwaysDiscardsLateVideoFrames=true
output.videoSettings=[kCVPixelBufferPixelFormatTypeKey as String:kCVPixelFormatType_32BGRA]
session.addOutput(output)
let writer=try AVAssetWriter(outputURL:URL(fileURLWithPath:args[1]),fileType:.mp4)
final class Capture:NSObject,AVCaptureVideoDataOutputSampleBufferDelegate {
 let writer:AVAssetWriter;let session:AVCaptureSession;let duration:Double
 let done=DispatchSemaphore(value:0)
 var input:AVAssetWriterInput?;var first:CMTime?;var finished=false;var count=0;var dropped=0
 var rows="video_s,desktop_monotonic_ns,callback_monotonic_ns\n"
 init(_ w:AVAssetWriter,_ s:AVCaptureSession,_ d:Double){writer=w;session=s;duration=d}
 func captureOutput(_ output:AVCaptureOutput,didOutput sample:CMSampleBuffer,from connection:AVCaptureConnection){
  guard !finished else{return}
  let pts=CMSampleBufferGetPresentationTimeStamp(sample)
  let now=mono()
  // Convert the capture timestamp from the session clock to host time, then
  // bridge host time to the same CLOCK_MONOTONIC used by the C++ operator.
  let host=CMClockGetHostTimeClock()
  let hp=CMSyncConvertTime(pts,from:session.masterClock ?? host,to:host)
  let captured=now+(CMTimeGetSeconds(hp)-CMTimeGetSeconds(CMClockGetTime(host)))*1e9
  if first == nil {
   guard let image=CMSampleBufferGetImageBuffer(sample) else{return}
   let settings:[String:Any]=[AVVideoCodecKey:AVVideoCodecType.h264,AVVideoWidthKey:CVPixelBufferGetWidth(image),AVVideoHeightKey:CVPixelBufferGetHeight(image),AVVideoCompressionPropertiesKey:[AVVideoAverageBitRateKey:8_000_000]]
   let i=AVAssetWriterInput(mediaType:.video,outputSettings:settings);i.expectsMediaDataInRealTime=true
   writer.add(i);input=i
   guard writer.startWriting() else{print("Writer error \(String(describing:writer.error))");finished=true;done.signal();return}
   writer.startSession(atSourceTime:pts);first=pts
   print("RECORDING \(args[1])");fflush(stdout)
  }
  let elapsed=CMTimeGetSeconds(CMTimeSubtract(pts,first!))
  if elapsed>=duration {
   finished=true;input!.markAsFinished();writer.finishWriting { self.done.signal() };return
  }
  guard input!.isReadyForMoreMediaData else{dropped+=1;return}
  if input!.append(sample) {rows+=String(format:"%.9f,%.0f,%.0f\n",elapsed,captured,now);count+=1}
  else {finished=true;print("Append error \(String(describing:writer.error))");done.signal()}
 }
}
let delegate=Capture(writer,session,duration)
let queue=DispatchQueue(label:"robot.timed.camera")
output.setSampleBufferDelegate(delegate,queue:queue)
session.startRunning()
let status=delegate.done.wait(timeout:.now()+duration+25)
session.stopRunning()
queue.sync {
 try? delegate.rows.write(toFile:args[1]+".frames.csv",atomically:true,encoding:.utf8)
 print("SAVED frames=\(delegate.count) writer_dropped=\(delegate.dropped) status=\(writer.status.rawValue)")
}
if status == .timedOut || writer.status != .completed {exit(5)}
