/*
 Copyright (c) 2013 yvt
 
 This file is part of OpenSpades.
 
 OpenSpades is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 OpenSpades is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with OpenSpades.  If not, see <http://www.gnu.org/licenses/>.
 
 */

#include "SWImageRenderer.h"
#include <Core/Bitmap.h>
#include "SWImage.h"

namespace spades {
	namespace draw {
		SWImageRenderer::SWImageRenderer(SWFeatureLevel lvl):
		shader(ShaderType::Image),
		featureLevel(lvl){
			
		}
		
		SWImageRenderer::~SWImageRenderer() {
			
		}
		
		void SWImageRenderer::SetFramebuffer(spades::Bitmap *bmp) {
			this->frame = bmp;
			if(bmp) {
				fbSize4 = MakeVector4(static_cast<float>(bmp->GetWidth()) * .5f,
									  static_cast<float>(bmp->GetHeight()) * .5f,
									  1.f, 1.f);
				fbCenter4 = MakeVector4(static_cast<float>(bmp->GetWidth()) * .5f,
										static_cast<float>(bmp->GetHeight()) * .5f,
										0.f, 0.f);
			}
		}
		
		void SWImageRenderer::SetDepthBuffer(float *f) {
			depthBuffer = f;
		}
		
		void SWImageRenderer::SetShaderType(ShaderType type) {
			shader = type;
		}
		
		void SWImageRenderer::SetZRange(float zNear, float) {
			// currently zNear is ignored...
			this->zNear = zNear;
		}
		
		
		struct Interpolator {
			int mode; // 0:fixed point, 1/-1: bresenham
			
			// fixed point
			struct {
				int64_t counter;
				int64_t step;
			} fp;
			
			struct {
				unsigned int divisor;
				unsigned int dividend;
				unsigned int step;
				int largePos;
			} b;
			
			static int abs(int v) {
				return v < 0 ? -v : v;
			}
			
			Interpolator(int start, int end, int numSteps, bool noBresenham = true) {
				// FIXME: same sub-pixel positioning as OpenGL?
				if(abs(end - start) <= numSteps && !noBresenham) {
					int distance = end - start;
					if(distance >= 0) {
						mode = 1;
					}else{
						mode = -1;
						distance = -distance;
					}
					if(numSteps == 0)
						numSteps = 1;
					b.divisor = static_cast<unsigned int>(numSteps);
					b.dividend = 0;
					b.largePos = start;
					b.step = static_cast<unsigned int>(distance);
				}else{
					mode = 0;
					if(numSteps == 0) {
						fp.step = 0;
					}else{
						fp.step = static_cast<int64_t>(end - start) << 32;
						fp.step /= static_cast<int64_t>(numSteps);
					}/*else if(end > start){
						unsigned int distance = end - start;
						unsigned int large = distance / static_cast<unsigned int>(numSteps);
						fp.step = static_cast<int64_t>(large) << 32;
						
						unsigned int distance2 = distance - static_cast<unsigned int>(numSteps) * large;
						unsigned int medium = (distance2 << 12) / static_cast<unsigned int>(numSteps);
						fp.step += static_cast<int64_t>(medium) << 20;
						
						unsigned int distance3 = (distance2 << 12) - static_cast<unsigned int>(numSteps) * medium;
						unsigned int small = (distance3 << 12) / static_cast<unsigned int>(numSteps);
						fp.step += static_cast<int64_t>(small) << 8;
						
						
					}else{
						unsigned int distance = start - end;
						unsigned int large = distance / static_cast<unsigned int>(numSteps);
						fp.step = static_cast<int64_t>(large) << 32;
						
						unsigned int distance2 = distance - static_cast<unsigned int>(numSteps) * large;
						unsigned int medium = (distance2 << 12) / static_cast<unsigned int>(numSteps);
						fp.step += static_cast<int64_t>(medium) << 20;
						
						unsigned int distance3 = (distance2 << 12) - static_cast<unsigned int>(numSteps) * medium;
						unsigned int small = (distance3 << 12) / static_cast<unsigned int>(numSteps);
						fp.step += static_cast<int64_t>(small) << 8;
						
						fp.step = -fp.step;
					}*/
					fp.counter = static_cast<int64_t>(start) << 32;
				}
			}
			int GetCurrent() {
				if(mode != 0)
					return b.largePos;
				return static_cast<int>(fp.counter >> 32);
			}
			void MoveNext() {
				if(mode == 0) {
					fp.counter += fp.step;
				}else{
					b.dividend += b.step;
					while(b.dividend >= b.divisor){
						b.dividend -= b.divisor;
						b.largePos += mode;
					}
				}
			}
			void MoveNext(int numSteps) {
				if(mode == 0) {
					if(numSteps == 1)
						MoveNext();
					else if(numSteps == 0)
						return;
					else
						fp.counter += fp.step * static_cast<int64_t>(numSteps);
				}else{
					if(numSteps < 4){
						while(numSteps--)
							MoveNext();
					}else{
						unsigned long long d = b.dividend;
						d += static_cast<unsigned long long>(b.step * static_cast<unsigned int>(numSteps));
						unsigned long long cnt = d / b.divisor;
						d -= cnt * b.divisor;
						b.dividend = static_cast<unsigned int>(d);
						b.largePos += mode * static_cast<int>(cnt);
					}
				}
			}
		};
		
		static constexpr int texUVScaleBits = 18;
		static constexpr int texUVScaleInt = 1 << texUVScaleBits;
		static constexpr float texUVScaleFloat = static_cast<float>(texUVScaleInt);
		
		struct SWImageVarying {
			union {
				struct {
					int u, v;
				};
#if ENABLE_SSE2
				__m128i uv_m128; // [u32, ?32, v32, ?32]
#endif
			};
#if __cplusplus >= 201103L
			SWImageVarying() = default; // POD
#else
			SWImageVarying() {} // non-POD
#endif
			SWImageVarying(int u, int v):
			u(u), v(v){ }
			
			SWImageVarying(const SWImageRenderer::Vertex& v):
			u(static_cast<int>(v.uv.x * texUVScaleFloat + .5f)),
			v(static_cast<int>(v.uv.y * texUVScaleFloat + .5f)) {
				
			}
		};
		
		template<SWFeatureLevel level>
		struct SWImageGouraudInterpolator {
			Interpolator u, v;
			SWImageGouraudInterpolator(const SWImageVarying& start,
									   const SWImageVarying& end,
									   int numSteps):
			u(start.u, end.u, numSteps),
			v(start.v, end.v, numSteps)
			{}
			
			SWImageVarying GetCurrent() {
				return SWImageVarying(u.GetCurrent(), v.GetCurrent());
			}
			
			void MoveNext(int s) {
				u.MoveNext(s);
				v.MoveNext(s);
			}
			
			void MoveNext() {
				u.MoveNext();
				v.MoveNext();
			}
		};
		
#if ENABLE_SSE2
		template<>
		struct SWImageGouraudInterpolator<SWFeatureLevel::SSE2> {
			Interpolator u, v;
			
			union{
				__m128i uv;
				struct {
					int64_t uvU, uvV;
				};
			};
			union {
				__m128i uvStep;
				struct {
					int64_t stepU, stepV;
				};
			};
			
			SWImageGouraudInterpolator(const SWImageVarying& start,
									   const SWImageVarying& end,
									   int numSteps):
			u(start.u, end.u, numSteps, true),
			v(start.v, end.v, numSteps, true)
			{
				uv = _mm_set_epi64x(u.fp.counter, v.fp.counter);
				uvStep = _mm_set_epi64x(u.fp.step, v.fp.step);
			}
			
			SWImageVarying GetCurrent() {
				SWImageVarying varying;
				varying.uv_m128 = _mm_shuffle_epi32(uv, 0b00111101);
				return varying;
			}
			
			void MoveNext(int s) {
				if(s == 0) return;
				else if(s < 4){
					auto v = uv, st = uvStep;
					while(s--) {
						v = _mm_add_epi64(v, st);
					}
					uv = v;
				}
				else {
					// no SSE2 support for 64bit multiply, but
					// this isn't a big problem because this case is rare
					uvU += stepU * s;
					uvV += stepV * s;
				}
			}
			
			void MoveNext() {
				uv = _mm_add_epi64(uv, uvStep);
			}
		};
#endif
		
		
#pragma mark - Polygon Renderer Main
		
		template<
		SWFeatureLevel level,
		bool needTransform,
		bool ndc, // normalized device coordinate
		bool depthTest
		>
		struct SWImageRenderer::PolygonRenderer {
			
			static_assert(!needTransform, "Transform pass was not selected");
			static_assert(!ndc, "Denormalize pass was not selected");
			
			static void DrawPolygonInternalInner(SWImage *img,
											const Vertex& v1,
											const Vertex& v2,
											const Vertex& v3,
											SWImageRenderer& r) {
				
				
				Bitmap *const fb = r.frame;
				SPAssert(fb != nullptr);
				
				if(v3.position.y <= 0.f) {
					// viewport cull
					return;
				}
				
				const int fbW = fb->GetWidth();
				const int fbH = fb->GetHeight();
				uint32_t *const bmp = fb->GetPixels();
				
				if(v1.position.y >= static_cast<float>(fbH)) {
					// viewport cull
					return;
				}
				
				float *const depthBuffer = r.depthBuffer;
				if(depthTest){
					SPAssert(depthBuffer != nullptr);
				}
				
				Bitmap *const tbmp = img->GetRawBitmap();
				const int tw = tbmp->GetWidth();
				const int th = tbmp->GetHeight();
				const uint32_t * const tpixels = tbmp->GetPixels();
				
				const int x1 = static_cast<int>(v1.position.x);
				const int y1 = static_cast<int>(v1.position.y);
				const int x2 = static_cast<int>(v2.position.x);
				const int y2 = static_cast<int>(v2.position.y);
				const int x3 = static_cast<int>(v3.position.x);
				const int y3 = static_cast<int>(v3.position.y);
			
				if(x1 == x2 && x2 == x3) return; // area cull
				if(y1 == y3) return; // area cull
				if(std::min(std::min(x1, x2), x3) >= fbW) return; // viewport cull
				if(std::max(std::max(x1, x2), x3) <= 0) return; // viewport cull
				
				auto convertColor = [](float f) {
					int i = static_cast<int>(f * 256.f + .5f);
					return static_cast<unsigned short>(std::max(std::min(i, 256), 0));
				};
				unsigned short mulR = convertColor(v1.color.x);
				unsigned short mulG = convertColor(v1.color.y);
				unsigned short mulB = convertColor(v1.color.z);
				unsigned short mulA = convertColor(v1.color.w);
				
				if(mulA == 0 && mulR == 0 && mulG == 0 && mulB == 0)
					return;
				
				auto drawPixel = [mulR, mulG, mulB, mulA](uint32_t& dest, float& destDepth,
														  uint32_t texture, float inDepth) {
					if(depthTest) {
						if(inDepth < destDepth) {
							return;
						}
					}
					
					unsigned int ta = static_cast<unsigned int>(texture >> 24);
					ta += (ta >> 7); // [0, 255] -> [0, 256]
					texture = ((((texture & 0xff00ff) * ta) & 0xff00ff00) |
					(((texture & 0x00ff00) * ta) & 0x00ff0000)) >> 8; // premultiply
					unsigned int tr = static_cast<unsigned int>((texture >> 0) & 0xff);
					unsigned int tg = static_cast<unsigned int>((texture >> 8) & 0xff);
					unsigned int tb = static_cast<unsigned int>((texture >>16) & 0xff);
					tr = (tr * mulR) >> 8; tg = (tg * mulG) >> 8;
					tb = (tb * mulB) >> 8; ta = (ta * mulA) >> 8;
					
					uint32_t destCol = dest;
					unsigned int dr = static_cast<unsigned int>((destCol >> 0) & 0xff);
					unsigned int dg = static_cast<unsigned int>((destCol >> 8) & 0xff);
					unsigned int db = static_cast<unsigned int>((destCol >>16) & 0xff);
					unsigned int invA = 256 - ta;
					dr = (dr * invA) >> 8; dg = (dg * invA) >> 8;
					db = (db * invA) >> 8;
					
					unsigned int outR = tr + dr, outG = tg + dg, outB = tb + db;
					outR = std::min(outR, 255U);
					outG = std::min(outG, 255U);
					outB = std::min(outB, 255U);
					
					dest = outR | (outG << 8) | (outB << 16);
				};
				
				auto drawScanline = [tw, th, tpixels, bmp, fbW, fbH, depthBuffer, &drawPixel]
				(int y, int x1, int x2,
				 const SWImageVarying& vary1,
				 const SWImageVarying& vary2,
				 float z1, float z2) {
					uint32_t *out = bmp + (y * fbW);
					float *depthOut = nullptr;
					if(depthTest) {
						depthOut = depthBuffer + (y * fbW);
					}
					SPAssert(x1 < x2);
					int width = x2 - x1;
					SWImageGouraudInterpolator<level> vary(vary1, vary2, width);
					int minX = std::max(x1, 0);
					int maxX = std::min(x2, fbW);
					vary.MoveNext(minX - x1);
					out += minX;
					if(depthTest) {
						depthOut += minX;
					}
					for(int x = minX; x < maxX; x++) {
						auto vr = vary.GetCurrent();
						unsigned int u = static_cast<unsigned int>(vr.u & (texUVScaleInt-1));
						unsigned int v = static_cast<unsigned int>(vr.v & (texUVScaleInt-1));
						u = (u * tw) >> texUVScaleBits;
						v = (v * th) >> texUVScaleBits;
						uint32_t tex = tpixels[u + v * tw];
						// FIXME: Z interpolation
						// FIXME: perspective correction
						drawPixel(*out, *depthOut, tex, z1);
						out++;
						if(depthTest) {
							depthOut ++;
						}
						vary.MoveNext();
					}
				};
				
				// FIXME: interpolated Z
				
				Interpolator longSpanX(x1, x3, y3 - y1);
				SWImageGouraudInterpolator<level> longSpan(v1, v3, y3 - y1);
				{
					Interpolator shortSpanX(x1, x2, y2 - y1);
					SWImageGouraudInterpolator<level> shortSpan(v1, v2, y2 - y1);
					int minY = std::max(0, y1);
					int maxY = std::min(fbH, y2);
					shortSpanX.MoveNext(minY - y1);
					shortSpan.MoveNext(minY - y1);
					longSpanX.MoveNext(minY - y1);
					longSpan.MoveNext(minY - y1);
					for(int y = minY; y < maxY; y++) {
						int lineX1 = shortSpanX.GetCurrent();
						auto line1 = shortSpan.GetCurrent();
						int lineX2 = longSpanX.GetCurrent();
						auto line2 = longSpan.GetCurrent();
						shortSpanX.MoveNext();
						shortSpan.MoveNext();
						longSpanX.MoveNext();
						longSpan.MoveNext();
						if(lineX1 == lineX2) continue;
						if(lineX1 < lineX2) {
							drawScanline(y, lineX1, lineX2, line1, line2,
										 v1.position.z, v1.position.z);
						}else{
							drawScanline(y, lineX2, lineX1, line2, line1,
										 v1.position.z, v1.position.z);
						}
					}
				}
				{
					Interpolator shortSpanX(x2, x3, y3 - y2);
					SWImageGouraudInterpolator<level> shortSpan(v2, v3, y3 - y2);
					int minY = std::max(0, y2);
					int maxY = std::min(fbH, y3);
					shortSpanX.MoveNext(minY - y2);
					shortSpan.MoveNext(minY - y2);
					longSpanX.MoveNext(minY - y2);
					longSpan.MoveNext(minY - y2);
					for(int y = minY; y < maxY; y++) {
						int lineX1 = shortSpanX.GetCurrent();
						auto line1 = shortSpan.GetCurrent();
						int lineX2 = longSpanX.GetCurrent();
						auto line2 = longSpan.GetCurrent();
						shortSpanX.MoveNext();
						shortSpan.MoveNext();
						longSpanX.MoveNext();
						longSpan.MoveNext();
						if(lineX1 == lineX2) continue;
						if(lineX1 < lineX2) {
							drawScanline(y, lineX1, lineX2, line1, line2,
										 v1.position.z, v1.position.z);
						}else{
							drawScanline(y, lineX2, lineX1, line2, line1,
										 v1.position.z, v1.position.z);
						}
					}
				}
				// polygon, done!
			}
			
			static void DrawPolygonInternal(SWImage *img,
											const Vertex& v1,
											const Vertex& v2,
											const Vertex& v3,
											SWImageRenderer& r) {
				if(v2.position.y < v1.position.y) {
					if(v3.position.y < v2.position.y) {
						DrawPolygonInternalInner(img, v3, v2, v1, r);
					}else if(v3.position.y < v1.position.y) {
						DrawPolygonInternalInner(img, v2, v3, v1, r);
					}else{
						DrawPolygonInternalInner(img, v2, v1, v3, r);
					}
				}else if(v3.position.y < v1.position.y){
					DrawPolygonInternalInner(img, v3, v1, v2, r);
				}else if(v3.position.y < v2.position.y){
					DrawPolygonInternalInner(img, v1, v3, v2, r);
				}else{
					DrawPolygonInternalInner(img, v1, v2, v3, r);
				}
			}
		};
		
#pragma mark - SSE2
		
#if ENABLE_SSE2
		template<
		bool depthTest
		>
		struct SWImageRenderer::PolygonRenderer
		<SWFeatureLevel::SSE2, false, false, depthTest> {
			
			
			static void DrawPolygonInternalInner(SWImage *img,
												 const Vertex& v1,
												 const Vertex& v2,
												 const Vertex& v3,
												 SWImageRenderer& r) {
				
				
				Bitmap *const fb = r.frame;
				SPAssert(fb != nullptr);
				
				if(v3.position.y <= 0.f) {
					// viewport cull
					return;
				}
				
				const int fbW = fb->GetWidth();
				const int fbH = fb->GetHeight();
				uint32_t *const bmp = fb->GetPixels();
				
				if(v1.position.y >= static_cast<float>(fbH)) {
					// viewport cull
					return;
				}
				
				float *const depthBuffer = r.depthBuffer;
				if(depthTest){
					SPAssert(depthBuffer != nullptr);
				}
				
				Bitmap *const tbmp = img->GetRawBitmap();
				const int tw = tbmp->GetWidth();
				const int th = tbmp->GetHeight();
				const uint32_t * const tpixels = tbmp->GetPixels();
				
				const int x1 = static_cast<int>(v1.position.x);
				const int y1 = static_cast<int>(v1.position.y);
				const int x2 = static_cast<int>(v2.position.x);
				const int y2 = static_cast<int>(v2.position.y);
				const int x3 = static_cast<int>(v3.position.x);
				const int y3 = static_cast<int>(v3.position.y);
				
				if(x1 == x2 && x2 == x3) return; // area cull
				if(y1 == y3) return; // area cull
				if(std::min(std::min(x1, x2), x3) >= fbW) return; // viewport cull
				if(std::max(std::max(x1, x2), x3) <= 0) return; // viewport cull
				
				auto convertColor = [](float f) {
					int i = static_cast<int>(f * 256.f + .5f);
					return static_cast<unsigned short>(std::max(std::min(i, 256), 0));
				};
				unsigned short mulR = convertColor(v1.color.x);
				unsigned short mulG = convertColor(v1.color.y);
				unsigned short mulB = convertColor(v1.color.z);
				unsigned short mulA = convertColor(v1.color.w);
				
				if(mulA == 0 && mulR == 0 && mulG == 0 && mulB == 0)
					return;
				
				mulR <<= 7;
				mulG <<= 7;
				mulB <<= 7;
				__m128i mulCol = _mm_setr_epi16(mulR, mulG, mulB, mulA,
												mulR, mulG, mulB, mulA);
				
				auto drawPixel = [mulCol]
				(uint32_t& dest, float& destDepth,
				 uint32_t texture, float inDepth) {
					if(depthTest) {
						if(inDepth < destDepth) {
							return;
						}
					}
					
					unsigned int ta = static_cast<unsigned int>(texture >> 24);
					ta += (ta >> 7); // [0, 255] -> [0, 256]
					
					// load [u8.0x4]
					__m128i tcol = _mm_setr_epi32(texture | 0xff000000, 0,0,0);
					
					// convert to [u16.0x4], 8bit width
					tcol = _mm_unpacklo_epi8(tcol, _mm_setzero_si128());
					
					__m128i taVec = _mm_set1_epi16(static_cast<short>(ta));
					
					// premultiply. now [u8.8x4]
					tcol = _mm_mullo_epi16(tcol, taVec);
					
					// modulate by the constant color. now [u9.7x4], 8bit width
					tcol = _mm_mulhi_epu16(tcol, mulCol);
					
					// broadcast the alpha of the tcol.
					__m128i tcolAlphaVec = _mm_shufflelo_epi16(tcol, 0b11111111);
					
					// make tcol [u8.8x4]
					tcol = _mm_slli_epi16(tcol, 1);
					
					tcolAlphaVec = _mm_add_epi16(tcolAlphaVec,
												 _mm_srli_epi16(tcolAlphaVec, 7)); // [0,255] -> [0,256]
					
					// inverse the alpha
					tcolAlphaVec = _mm_sub_epi16(_mm_set1_epi16(0x100),
												 tcolAlphaVec);
					
					// load [u8.0x4]
					__m128i dcol = _mm_setr_epi32(dest, 0,0,0);
					
					// convert to [u16.0x4], 8bit width
					dcol = _mm_unpacklo_epi8(dcol, _mm_setzero_si128());
					
					// modulate by inversed src alpha.
					// now [u8.8 x 4]
					dcol = _mm_mullo_epi16(dcol, tcolAlphaVec);
					
					// additive blending with saturation.
					dcol = _mm_adds_epu16(dcol, tcol);
					
					// pack.
					dcol = _mm_srli_epi16(dcol, 8);
					dcol = _mm_packus_epi16(dcol, dcol);
					
					// store.
					_mm_store_ss(reinterpret_cast<float *>(&dest),
								 dcol);
				};
				
				auto drawScanline = [tw, th, tpixels, bmp, fbW, fbH, depthBuffer, &drawPixel]
				(int y, int x1, int x2,
				 const SWImageVarying& vary1,
				 const SWImageVarying& vary2,
				 float z1, float z2) {
					uint32_t *out = bmp + (y * fbW);
					float *depthOut = nullptr;
					if(depthTest) {
						depthOut = depthBuffer + (y * fbW);
					}
					SPAssert(x1 < x2);
					int width = x2 - x1;
					SWImageGouraudInterpolator<SWFeatureLevel::SSE2> vary(vary1, vary2, width);
					int minX = std::max(x1, 0);
					int maxX = std::min(x2, fbW);
					vary.MoveNext(minX - x1);
					out += minX;
					if(depthTest) {
						depthOut += minX;
					}
					auto uvMask = _mm_set1_epi32(texUVScaleInt - 1);
					auto uvScale = _mm_setr_epi32(tw, 0, th, 0);
					for(int x = minX; x < maxX; x++) {
						auto vr = vary.GetCurrent();
						union {
							__m128i uv;
							struct { unsigned int ui, dummy1, vi, dummy2; };
						};
						uv = vr.uv_m128;
						uv = _mm_and_si128(uv, uvMask); // repeat
						uv = _mm_mul_epu32(uv, uvScale); // now [u*tw, v*th]
						uv = _mm_srli_epi64(uv, texUVScaleBits);
						
						uint32_t tex = tpixels[ui + vi * tw];
						// FIXME: Z interpolation
						// FIXME: perspective correction
						drawPixel(*out, *depthOut, tex, z1);
						out++;
						if(depthTest) {
							depthOut ++;
						}
						vary.MoveNext();
					}
				};
				
				// FIXME: interpolated Z
				
				Interpolator longSpanX(x1, x3, y3 - y1);
				SWImageGouraudInterpolator<SWFeatureLevel::SSE2> longSpan(v1, v3, y3 - y1);
				{
					Interpolator shortSpanX(x1, x2, y2 - y1);
					SWImageGouraudInterpolator<SWFeatureLevel::SSE2> shortSpan(v1, v2, y2 - y1);
					int minY = std::max(0, y1);
					int maxY = std::min(fbH, y2);
					shortSpanX.MoveNext(minY - y1);
					shortSpan.MoveNext(minY - y1);
					longSpanX.MoveNext(minY - y1);
					longSpan.MoveNext(minY - y1);
					for(int y = minY; y < maxY; y++) {
						int lineX1 = shortSpanX.GetCurrent();
						auto line1 = shortSpan.GetCurrent();
						int lineX2 = longSpanX.GetCurrent();
						auto line2 = longSpan.GetCurrent();
						shortSpanX.MoveNext();
						shortSpan.MoveNext();
						longSpanX.MoveNext();
						longSpan.MoveNext();
						if(lineX1 == lineX2) continue;
						if(lineX1 < lineX2) {
							drawScanline(y, lineX1, lineX2, line1, line2,
										 v1.position.z, v1.position.z);
						}else{
							drawScanline(y, lineX2, lineX1, line2, line1,
										 v1.position.z, v1.position.z);
						}
					}
				}
				{
					Interpolator shortSpanX(x2, x3, y3 - y2);
					SWImageGouraudInterpolator<SWFeatureLevel::SSE2> shortSpan(v2, v3, y3 - y2);
					int minY = std::max(0, y2);
					int maxY = std::min(fbH, y3);
					shortSpanX.MoveNext(minY - y2);
					shortSpan.MoveNext(minY - y2);
					longSpanX.MoveNext(minY - y2);
					longSpan.MoveNext(minY - y2);
					for(int y = minY; y < maxY; y++) {
						int lineX1 = shortSpanX.GetCurrent();
						auto line1 = shortSpan.GetCurrent();
						int lineX2 = longSpanX.GetCurrent();
						auto line2 = longSpan.GetCurrent();
						shortSpanX.MoveNext();
						shortSpan.MoveNext();
						longSpanX.MoveNext();
						longSpan.MoveNext();
						if(lineX1 == lineX2) continue;
						if(lineX1 < lineX2) {
							drawScanline(y, lineX1, lineX2, line1, line2,
										 v1.position.z, v1.position.z);
						}else{
							drawScanline(y, lineX2, lineX1, line2, line1,
										 v1.position.z, v1.position.z);
						}
					}
				}
				// polygon, done!
			}
			
			static void DrawPolygonInternal(SWImage *img,
											const Vertex& v1,
											const Vertex& v2,
											const Vertex& v3,
											SWImageRenderer& r) {
				if(v2.position.y < v1.position.y) {
					if(v3.position.y < v2.position.y) {
						DrawPolygonInternalInner(img, v3, v2, v1, r);
					}else if(v3.position.y < v1.position.y) {
						DrawPolygonInternalInner(img, v2, v3, v1, r);
					}else{
						DrawPolygonInternalInner(img, v2, v1, v3, r);
					}
				}else if(v3.position.y < v1.position.y){
					DrawPolygonInternalInner(img, v3, v1, v2, r);
				}else if(v3.position.y < v2.position.y){
					DrawPolygonInternalInner(img, v1, v3, v2, r);
				}else{
					DrawPolygonInternalInner(img, v1, v2, v3, r);
				}
			}
		};
#endif
		
#pragma mark - Intermediates
		
		template<
		SWFeatureLevel featureLvl,
		bool depthTest
		>
		struct SWImageRenderer::PolygonRenderer<featureLvl, false, true, depthTest> {
			static void DrawPolygonInternal(SWImage *img,
											const Vertex& v1,
											const Vertex& v2,
											const Vertex& v3,
											SWImageRenderer& r) {
				
				// denormalize
				auto vv1 = v1, vv2 = v2, vv3 = v3;
				vv1.position = (vv1.position * r.fbSize4) + r.fbCenter4;
				vv2.position = (vv2.position * r.fbSize4) + r.fbCenter4;
				vv3.position = (vv3.position * r.fbSize4) + r.fbCenter4;
				PolygonRenderer<featureLvl, false, false, depthTest>::DrawPolygonInternal(img,
																   vv1, vv2, vv3, r);
			}
		};
		
		template<
		SWFeatureLevel featureLvl,
		bool ndc, // normalized device coordinate
		bool depthTest
		>
		struct SWImageRenderer::PolygonRenderer<featureLvl, true, ndc, depthTest> {
			template<class F>
			static void Clip(Vertex& v1, Vertex& v2, Vertex& v3,
							 Vector4 plane, F continuation) {
				auto distance = [](const Vector4& v, const Vector4& plane) {
					return v.x * plane.x + v.y * plane.y + v.z * plane.z + plane.w;
				};
				auto lerpVertex = [](const Vertex& v1, const Vertex& v2, Vertex& out,
									 float per) {
					out.position = v1.position + (v2.position - v1.position) * per;
					out.color = v1.color + (v2.color - v1.color) * per;
					out.uv = v1.uv + (v2.uv - v1.uv) * per;
				};
				float d1 = distance(v1.position, plane);
				float d2 = distance(v1.position, plane);
				float d3 = distance(v1.position, plane);
				bool nc1 = d1 >= 0.f;
				bool nc2 = d2 >= 0.f;
				bool nc3 = d3 >= 0.f;
				int bits = (nc1 ? 1 : 0) | (nc2 ? 2 : 0) | (nc3 ? 4 : 0);
				float per1, per2;
				Vertex vv1, vv2, vv3;
				Vertex t1, t2;
				switch(bits){
					case 0:
						// culled
						return;
					case 7:
						// not clipped
						continuation(v1, v2, v3);
						return;
					case 1:
						per1 = d2 / (d2 - d1); // == (0.f - d2) / (d1 - d2);
						per2 = d3 / (d3 - d1);
						lerpVertex(v2, v1, v2, per1);
						lerpVertex(v3, v1, v3, per2);
						continuation(v1, v2, v3);
						return;
					case 2:
						per1 = d1 / (d1 - d2);
						per2 = d3 / (d3 - d2);
						lerpVertex(v1, v2, v1, per1);
						lerpVertex(v3, v2, v3, per2);
						continuation(v1, v2, v3);
						return;
					case 4:
						per1 = d2 / (d2 - d3);
						per2 = d1 / (d1 - d3);
						lerpVertex(v2, v3, v2, per1);
						lerpVertex(v1, v3, v1, per2);
						continuation(v1, v2, v3);
						return;
					case 3:
						per1 = d2 / (d2 - d3);
						per2 = d1 / (d1 - d3);
						lerpVertex(v2, v3, t2, per1);
						lerpVertex(v1, v3, t1, per2);
						vv1 = v1;
						vv2 = v2;
						vv3 = t2;
						continuation(vv1, vv2, vv3);
						vv1 = v1;
						vv2 = t2;
						vv3 = t1;
						continuation(vv1, vv2, vv3);
						break;
					case 5:
						per1 = d3 / (d3 - d2);
						per2 = d1 / (d1 - d2);
						lerpVertex(v3, v2, t2, per1);
						lerpVertex(v1, v2, t1, per2);
						vv1 = v1;
						vv2 = v3;
						vv3 = t2;
						continuation(vv1, vv2, vv3);
						vv1 = v1;
						vv2 = t2;
						vv3 = t1;
						continuation(vv1, vv2, vv3);
						break;
					case 6:
						per1 = d3 / (d3 - d1);
						per2 = d2 / (d2 - d1);
						lerpVertex(v3, v1, t2, per1);
						lerpVertex(v2, v1, t1, per2);
						vv1 = v2;
						vv2 = v3;
						vv3 = t2;
						continuation(vv1, vv2, vv3);
						vv1 = v2;
						vv2 = t2;
						vv3 = t1;
						continuation(vv1, vv2, vv3);
						break;
				}
				
			}
			
			static void DrawPolygonInternal(SWImage *img,
											const Vertex& v1,
											const Vertex& v2,
											const Vertex& v3,
											SWImageRenderer& r) {
				// needs transform.
				auto vv1 = v1, vv2 = v2, vv3 = v3;
				const auto& mat = r.matrix;
				vv1.position = mat * vv1.position;
				vv2.position = mat * vv2.position;
				vv3.position = mat * vv3.position;
				
				Clip
				(vv1, vv2, vv3,
				 MakeVector4(0.f, 0.f, 1.f, 1.f),
				 [img,&r](Vertex& v1, Vertex& v2, Vertex& v3) {
					 Clip
					 (v1, v2, v3,
					  MakeVector4(0.f, 0.f, -1.f, 1.f),
					  [img,&r](Vertex& v1, Vertex& v2, Vertex& v3) {
						  PolygonRenderer<featureLvl, false, true, depthTest>::DrawPolygonInternal
						  (img, v1, v2, v3, r);
					  });
				 });
			}
		};
		
		
		template<
		bool needTransform,
		bool ndc, // normalized device coordinate
		bool depthTest
		>
		struct SWImageRenderer::PolygonRenderer2 {
			static void DrawPolygonInternal(SWImage *img,
											const Vertex& v1,
											const Vertex& v2,
											const Vertex& v3,
											SWImageRenderer& r,
											SWFeatureLevel lvl) {
#if ENABLE_SSE2
				if(lvl >= SWFeatureLevel::SSE2) {
					PolygonRenderer<SWFeatureLevel::SSE2, needTransform, ndc, depthTest>::DrawPolygonInternal(img,
																											  v1, v2, v3, r);
					return;
				}
#endif
				PolygonRenderer<SWFeatureLevel::None, needTransform, ndc, depthTest>::DrawPolygonInternal(img,
																										  v1, v2, v3, r);
			}
		};
		
		
		void SWImageRenderer::DrawPolygon(SWImage *img,
										  const Vertex& v1,
										  const Vertex& v2,
										  const Vertex& v3) {
			SPAssert(frame != nullptr);
			switch(shader){
				case ShaderType::Sprite:
					PolygonRenderer2<
					true,	// needs transform
					true,	// in NDC
					true	// depth tested
					>::DrawPolygonInternal(img, v1, v2, v3, *this, featureLevel);
					break;
				case ShaderType::Image:
					PolygonRenderer2<
					false,	// don't need transform
					false,	// not NDC
					false	// no depth test
					>::DrawPolygonInternal(img, v1, v2, v3, *this, featureLevel);
					break;
			}
		}
	}
}

