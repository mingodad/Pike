array funcEV = ({
  "glBegin",
  "glCullFace",
  "glDepthFunc",
  "glDisable",
  "glDisableClientState",
  "glDrawBuffer",
  "glEnable",
  "glEnableClientState",
  "glFrontFace",
  "glLogicOp",
  "glMatrixMode",
  "glReadBuffer",
  "glRenderMode",
  "glShadeModel",
});
array funcV = ({
  "glEnd",
  "glEndList",
  "glFinish",
  "glFlush",
  "glInitNames",
  "glLoadIdentity",
  "glPopAttrib",
  "glPopClientAttrib",
  "glPopMatrix",
  "glPopName",
  "glPushMatrix",
});
array func_misc = ({
  ({"glAccum", "VEF"}),
  ({"glAlphaFunc", "VEF"}),
  ({"glArrayElement", "VI"}),
  ({"glBindTexture","VEI"}),
  ({"glBlendFunc", "VEE"}),
  ({"glCallList","VI"}),
  ({"glClear","VB"}),
  ({"glClearAccum", "V+FFFF"}),
  ({"glClearColor", "V+FFFF"}),
  ({"glClearDepth", "VD"}),
  ({"glClearIndex", "VF"}),
  ({"glClearStencil", "VI"}),
  ({"glClipPlane", "VE=DDDD"}),
  ({"glColor", "V+ZZZZ"}),
  ({"glColorMask", "VOOOO"}),
  ({"glColorMaterial", "VEE"}),
  ({"glCopyPixels", "VIIIIE"}),
  ({"glCopyTexImage1D", "VEIEIIII"}),
  ({"glCopyTexImage2D", "VEIEIIIII"}),
  ({"glCopyTexSubImage1D", "VEIIIII"}),
  ({"glCopyTexSubImage2D", "VEIIIIIII"}),
  ({"glDeleteLists", "VII"}),
  ({"glDepthMask", "VO"}),
  ({"glDepthRange", "VDD"}),
  ({"glDrawArrays", "VEII"}),
  ({"glDrawPixels", "Vwhfti"}),
  ({"glEdgeFlag", "VO"}),
  ({"glEvalCoord", "V+RR"}),
  ({"glEvalPoint", "V+II"}),
  ({"glFog","VE@Q"}),
  ({"glFrustum", "VDDDDDD"}),
  ({"glGenLists", "II"}),
  ({"glGetError", "E"}),
  ({"glHint", "VEE"}),
  ({"glIndexMask", "VI"}),
  ({"glIsEnabled", "OE"}),
  ({"glIsList", "OI"}),
  ({"glIsTexture", "OI"}),
  ({"glLight", "VEE@Q"}),
  ({"glLightModel", "VE@Q"}),
  ({"glLineStipple", "VII"}),
  ({"glLineWidth", "VF"}),
  ({"glListBase", "VI"}),
  ({"glLoadName", "VI"}),
  ({"glMaterial", "VEE@Q"}),
  ({"glNewList", "VIE"}),
  ({"glNormal", "V#ZZZ"}),
  ({"glOrtho", "VDDDDDD"}),
  ({"glPassThrough", "VF"}),
  ({"glPixelZoom", "VFF"}),
  ({"glPointSize", "VF"}),
  ({"glPolygonMode", "VEE"}),
  ({"glPolygonOffset", "VFF"}),
  ({"glPushAttrib", "VB"}),
  ({"glPushClientAttrib", "VB"}),
  ({"glPushName", "VI"}),
  ({"glRasterPos", "V+ZZZ"}),
  ({"glRotate", "V!RRRR"}),
  ({"glScale", "V!RRR"}),
  ({"glScissor", "VIIII"}),
  ({"glStencilFunc", "VEII"}),
  ({"glStencilMask", "VI"}),
  ({"glStencilOp", "VEEE"}),
  ({"glTexCoord", "V+Z"}),
  ({"glTexEnv","VEE@Q"}),
  ({"glTexGen","VEE@Z"}),
  ({"glTexImage2D","VEIIwhIfti"}),
  ({"glTexParameter","VEE@Q"}),
  ({"glTexSubImage2D","VEIIIwhfti"}),
  ({"glTranslate", "V!RRR"}),
  ({"glVertex","V+ZZZ"}),
  ({"glViewport", "VIIII"}),
});
mapping func_cat = ([
  "VE":funcEV,
  "V":funcV,
]);

/*
  Not implemented:

  glAreTexturesResident
  glBitmap
  glCallLists
  glColorPointer
  glDeleteTextures
  glDrawElements
  glEdgeFlagPointer
  glEvalMesh
  glFeedbackBuffer
  glGenTextures
  glGetClipPlane
  glGetLight
  glGetMap
  glGetMaterial
  glGetPixelMap
  glGetPointer
  glGetPolygonStipple
  glGetString
  glGetTexEnv
  glGetTexGen
  glGetTexImage
  glGetTexLevelParameter
  glGetTexParameter
  glIndex
  glIndexPointer
  glInterleavedArrays
  glLoadMatrix
  glMap1
  glMap2
  glMapGrid
  glMultMatrix
  glNormalPointer
  glPixelMap
  glPixelTransfer
  glPolygonStipple
  glPrioritizeTextures
  glReadPixels
  glRect
  glSelectBuffer
  glTexCoordPointer
  glTexImage1D
  glTexSubImage1D
  glVertexPoint

*/
