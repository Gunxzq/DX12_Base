# Ambient Occlusion(环境光遮挡)
物体的表面从个别角度上来看，会被自身的部分给遮挡，基于透射光线法，可以计算出物体本身的AO图。


## 静态场景AO
AO图只关心物体本身的遮挡关系，但是场景中的物体之间存在遮挡关系，如果是静态物体所构建的场景，可以烘培出一个静态场景AO图。

## 动态AO
动态的物体会影响静态物体的遮挡关系，需要在运行时动态计算。
1. SSAO
2. HBAO/HBAO+
3. GTAO
4. VXAO
5. RTAO

##
1. normals：法线图
2. displacement：位移图
3. occlusion：遮挡图
4. specularity：高光图
5. diffuse：漫反射图
