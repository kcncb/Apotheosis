#pragma once
// Geometry-only stand-ins for running the production AimEngine on machines
// without OpenCV. No capture, image processing or inference is simulated here.
namespace cv {
template<class T> struct Point_ {T x=0,y=0;Point_()=default;Point_(T a,T b):x(a),y(b){}};
template<class T> struct Rect_ {T x=0,y=0,width=0,height=0;Rect_()=default;Rect_(T a,T b,T w,T h):x(a),y(b),width(w),height(h){}};
using Point2f=Point_<float>;using Rect2f=Rect_<float>;
}
