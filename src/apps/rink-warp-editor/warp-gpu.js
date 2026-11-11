/* WebGL2 inverse sampling. Only edits redraw; no video decoding or readback. */
(function (root) {
  "use strict";
  const vertex = `#version 300 es
  in vec2 position; void main() { gl_Position = vec4(position, 0., 1.); }`;
  const fragment = `#version 300 es
  precision highp float;
  uniform highp sampler2D photo;
  uniform highp sampler2D columns;
  uniform vec2 resolution;
  uniform vec4 imageRect;
  uniform vec2 sourceSize;
  uniform vec2 outputSize;
  uniform float bottomTarget;
  uniform float strength;
  uniform int lutSize;
  uniform bool warped;
  uniform bool maskView;
  out vec4 color;
  float segment(float z, float x0, float x1, float y0, float y1, float d0, float d1) {
    float s = (y1-y0)/(x1-x0), t = clamp((z-x0)/(x1-x0),0.,1.), p=t*(1.-t);
    return y0+(y1-y0)*(s*t*t+d0*p)/(s+(d0+d1-2.*s)*p);
  }
  void main() {
    vec2 p = vec2(gl_FragCoord.x, resolution.y-gl_FragCoord.y);
    vec2 uv = (p-imageRect.xy)/imageRect.zw;
    if (any(lessThan(uv,vec2(0.))) || any(greaterThan(uv,vec2(1.)))) { color=vec4(.035,.047,.065,1.); return; }
    // Normalized coordinates refer to first/last pixel centers, as the CPU renderer does.
    uv = (uv * outputSize - .5) / (outputSize - 1.);
    uv = clamp(uv,0.,1.);
    if (warped) {
      float index=uv.x*float(lutSize-1); int i=min(int(index),lutSize-2);
      vec4 c=mix(texelFetch(columns,ivec2(i,0),0),texelFetch(columns,ivec2(i+1,0),0),index-float(i));
      float up=c.y, low=c.z, top=c.w, bottom=mix(low,bottomTarget,strength);
      float first=up/top, middle=(low-up)/(bottom-top), shared=2.*first*middle/(first+middle);
      float end=min(1.,low+(1.-bottom)*middle);
      float y=uv.y;
      if(y<top) y=segment(y,0.,top,0.,up,first,shared);
      else if(y<bottom) y=segment(y,top,bottom,up,low,shared,middle);
      else y=segment(y,bottom,1.,low,end,middle,middle);
      uv=vec2(c.x,y);
    }
    vec2 sampleUV=(uv*(sourceSize-1.)+.5)/sourceSize;
    color=texture(photo,sampleUV);
    if(maskView) color=vec4(.12,.9,.65,color.r*.32);
  }`;
  class Renderer {
    constructor(canvas) {
      this.canvas = canvas;
      const gl = canvas.getContext("webgl2", {
        alpha: false,
        antialias: false,
        preserveDrawingBuffer: true,
      });
      if (!gl)
        throw Error(
          "WebGL2 is unavailable. Enable browser hardware acceleration and reopen this editor.",
        );
      this.gl = gl;
      const compile = (type, code) => {
        const s = gl.createShader(type);
        gl.shaderSource(s, code);
        gl.compileShader(s);
        if (!gl.getShaderParameter(s, gl.COMPILE_STATUS))
          throw Error(gl.getShaderInfoLog(s));
        return s;
      };
      const p = gl.createProgram(),
        vs = compile(gl.VERTEX_SHADER, vertex),
        fs = compile(gl.FRAGMENT_SHADER, fragment);
      gl.attachShader(p, vs);
      gl.attachShader(p, fs);
      gl.linkProgram(p);
      gl.deleteShader(vs);
      gl.deleteShader(fs);
      if (!gl.getProgramParameter(p, gl.LINK_STATUS))
        throw Error(gl.getProgramInfoLog(p));
      this.program = p;
      gl.useProgram(p);
      const b = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, b);
      gl.bufferData(
        gl.ARRAY_BUFFER,
        new Float32Array([-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1]),
        gl.STATIC_DRAW,
      );
      const at = gl.getAttribLocation(p, "position");
      gl.enableVertexAttribArray(at);
      gl.vertexAttribPointer(at, 2, gl.FLOAT, false, 0, 0);
      this.uniforms = {};
      for (const name of [
        "photo",
        "columns",
        "resolution",
        "imageRect",
        "sourceSize",
        "outputSize",
        "bottomTarget",
        "strength",
        "lutSize",
        "warped",
        "maskView",
      ])
        this.uniforms[name] = gl.getUniformLocation(p, name);
      this.photo = this.texture(0, gl.LINEAR);
      this.map = this.texture(1, gl.NEAREST);
      this.maxTexture = gl.getParameter(gl.MAX_TEXTURE_SIZE);
      this.maxRender = Math.min(
        gl.getParameter(gl.MAX_RENDERBUFFER_SIZE),
        ...gl.getParameter(gl.MAX_VIEWPORT_DIMS),
      );
      gl.uniform1i(this.uniforms.photo, 0);
      gl.uniform1i(this.uniforms.columns, 1);
      gl.uniform1i(this.uniforms.maskView, 0);
      const debug = gl.getExtension("WEBGL_debug_renderer_info");
      this.device = debug
        ? gl.getParameter(debug.UNMASKED_RENDERER_WEBGL)
        : gl.getParameter(gl.RENDERER);
    }
    texture(unit, filter) {
      const gl = this.gl,
        t = gl.createTexture();
      gl.activeTexture(gl.TEXTURE0 + unit);
      gl.bindTexture(gl.TEXTURE_2D, t);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      return t;
    }
    setImage(image, limit) {
      const gl = this.gl,
        width = image.width || image.naturalWidth,
        height = image.height || image.naturalHeight;
      const scale = Math.min(
        1,
        limit / width,
        limit / height,
        this.maxTexture / width,
        this.maxTexture / height,
      );
      let input = image;
      if (scale < 1) {
        input = document.createElement("canvas");
        input.width = Math.max(2, Math.round(width * scale));
        input.height = Math.max(2, Math.round(height * scale));
        input
          .getContext("2d")
          .drawImage(image, 0, 0, input.width, input.height);
      }
      this.imageSize = [
        input.width || input.naturalWidth,
        input.height || input.naturalHeight,
      ];
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, this.photo);
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
      gl.texImage2D(
        gl.TEXTURE_2D,
        0,
        gl.RGBA,
        gl.RGBA,
        gl.UNSIGNED_BYTE,
        input,
      );
      if (gl.getError() !== gl.NO_ERROR)
        throw Error(
          "The GPU could not upload this image. Try a smaller still.",
        );
    }
    setWarp(warp) {
      this.warp = warp;
      this.lutSize = Math.min(warp.spec.source_size[0], this.maxTexture);
      const gl = this.gl;
      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, this.map);
      gl.texImage2D(
        gl.TEXTURE_2D,
        0,
        gl.RGBA32F,
        this.lutSize,
        1,
        0,
        gl.RGBA,
        gl.FLOAT,
        warp.lut(this.lutSize),
      );
    }
    draw(rect, warped) {
      if (!this.imageSize || !this.warp) return;
      const gl = this.gl,
        u = this.uniforms,
        s = this.warp.spec;
      gl.viewport(0, 0, this.canvas.width, this.canvas.height);
      gl.useProgram(this.program);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, this.photo);
      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, this.map);
      gl.uniform2f(u.resolution, this.canvas.width, this.canvas.height);
      gl.uniform4fv(u.imageRect, rect);
      gl.uniform2fv(u.sourceSize, this.imageSize);
      gl.uniform2fv(u.outputSize, warped ? this.warp.size : s.source_size);
      gl.uniform1f(u.bottomTarget, s.target_rows[1]);
      gl.uniform1f(u.strength, this.warp.strength);
      gl.uniform1i(u.lutSize, this.lutSize);
      gl.uniform1i(u.warped, warped ? 1 : 0);
      gl.drawArrays(gl.TRIANGLES, 0, 6);
    }
    dispose() {
      this.gl.getExtension("WEBGL_lose_context")?.loseContext();
    }
  }
  root.RinkWarpRenderer = Renderer;
})(globalThis);
