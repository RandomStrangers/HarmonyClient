#include "Camera.h"
#include "ExtMath.h"
#include "Game.h"
#include "Window.h"
#include "Graphics.h"
#include "Funcs.h"
#include "Gui.h"
#include "Entity.h"
#include "Input.h"
#include "InputHandler.h"
#include "Event.h"
#include "Options.h"
#include "Picking.h"
#include "Platform.h"

struct _CameraData Camera;
static struct RayTracer cameraClipPos;
static Vec2 cam_rotOffset;
static cc_bool cam_isForwardThird;

static struct CameraState {
	float deltaX, deltaY;
} states[MAX_LOCAL_PLAYERS];

static void Camera_OnRawMovement(float deltaX, float deltaY, int deviceIndex) {
	int i = Game_MapState(deviceIndex);
	states[i].deltaX += deltaX; 
	states[i].deltaY += deltaY;
}

void Camera_KeyLookUpdate(float delta) {
	float amount;
	int i;
	if (Gui.InputGrab) return;

	/* divide by 25 to have reasonable sensitivity for default mouse sens */
	amount = (Camera.Sensitivity / 25.0f) * (1000 * delta);
	i = Game.CurrentState;

	if (Bind_IsTriggered[BIND_LOOK_UP])    states[i].deltaY -= amount;
	if (Bind_IsTriggered[BIND_LOOK_DOWN])  states[i].deltaY += amount;
	if (Bind_IsTriggered[BIND_LOOK_LEFT])  states[i].deltaX -= amount;
	if (Bind_IsTriggered[BIND_LOOK_RIGHT]) states[i].deltaX += amount;
}

/*########################################################################################################################*
*--------------------------------------------------Perspective camera-----------------------------------------------------*
*#########################################################################################################################*/
static void PerspectiveCamera_GetProjection(struct Matrix* proj) {
	float fovy = Camera.Fov * MATH_DEG2RAD;
	float aspectRatio = (float)Game.Width / (float)Game.Height;
	Gfx_CalcPerspectiveMatrix(proj, fovy, aspectRatio, (float)Game_ViewDistance);
}

static void PerspectiveCamera_GetView(struct Matrix* mat) {
	Vec3 pos = Camera.CurrentPos;
	Vec2 rot = Camera.Active->GetOrientation();

	Matrix_LookRot(mat, pos, rot);
	if (Game_ViewBobbing) Matrix_MulBy(mat, &Camera.TiltM);
}

static void PerspectiveCamera_GetPickedBlock(struct RayTracer* t) {
	struct LocalPlayer* p = Entities.CurPlayer;
	struct Entity* e      = &p->Base;

	Vec3 dir    = Vec3_GetDirVector(e->Yaw * MATH_DEG2RAD, e->Pitch * MATH_DEG2RAD + Camera.TiltPitch);
	Vec3 eyePos = Entity_GetEyePosition(e);
	Picking_CalcPickedBlock(&eyePos, &dir, p->ReachDistance, t);
}

#define CAMERA_SENSI_FACTOR (0.0002f / 3.0f * MATH_RAD2DEG)

static Vec2 PerspectiveCamera_GetMouseDelta(float delta) {
	float sensitivity = CAMERA_SENSI_FACTOR * Camera.Sensitivity;
	static float speedX, speedY, newSpeedX, newSpeedY, accelX, accelY;
	int i = Game.CurrentState;
	Vec2 v;

	if (Camera.Smooth) {
		accelX = (states[i].deltaX - speedX) * 35 / Camera.Mass;
		accelY = (states[i].deltaY - speedY) * 35 / Camera.Mass;
		newSpeedX = accelX * delta + speedX;
		newSpeedY = accelY * delta + speedY;

		/* High acceleration means velocity overshoots the correct position on low FPS, */
		/* causing wiggling. If newSpeed has opposite sign of speed, set speed to 0 */
		if (newSpeedX * speedX < 0) speedX = 0;
		else speedX = newSpeedX;
		if (newSpeedY * speedY < 0) speedY = 0;
		else speedY = newSpeedY;
	} else {
		speedX = states[i].deltaX;
		speedY = states[i].deltaY;
	}

	v.x = speedX * sensitivity; 
	v.y = speedY * sensitivity;
	if (Camera.Invert) v.y = -v.y;
	return v;
}

static void PerspectiveCamera_UpdateMouseRotation(struct LocalPlayer* p, float delta) {
	struct Entity* e = &p->Base;
	struct LocationUpdate update;
	Vec2 rot = PerspectiveCamera_GetMouseDelta(delta);

	if (Input_IsAltPressed() && Camera.Active->isThirdPerson) {
		cam_rotOffset.x += rot.x; 
		cam_rotOffset.y += rot.y;
		return;
	}
	
	update.flags = LU_HAS_YAW | LU_HAS_PITCH;
	update.yaw   = e->next.yaw   + rot.x;
	update.pitch = e->next.pitch + rot.y;
	update.pitch = Math_ClampAngle(update.pitch);

	/* Need to make sure we don't cross the vertical axes, because that gets weird. */
	if (update.pitch >= 90.0f && update.pitch <= 270.0f) {
		update.pitch = e->next.pitch < 180.0f ? 90.0f : 270.0f;
	}
	e->VTABLE->SetLocation(e, &update);
}

static void PerspectiveCamera_UpdateMouse(struct LocalPlayer* p, float delta) {
	int i = Game.CurrentState;
	if (!Gui.InputGrab && Window_Main.Focused) Window_UpdateRawMouse();

	PerspectiveCamera_UpdateMouseRotation(p, delta);
	states[i].deltaX = 0; 
	states[i].deltaY = 0;
}

static void PerspectiveCamera_CalcViewBobbing(struct LocalPlayer* p, float t, float velTiltScale) {
	struct Entity* e = &p->Base;
	struct Matrix tiltY, velX;
	float vel, fall;
	
	if (!Game_ViewBobbing) { 
		Camera.TiltM     = Matrix_Identity;
		Camera.TiltPitch = 0.0f;
		return; 
	}

	Matrix_RotateZ(&Camera.TiltM, -p->Tilt.TiltX                  * e->Anim.BobStrength);
	Matrix_RotateX(&tiltY,        Math_AbsF(p->Tilt.TiltY) * 3.0f * e->Anim.BobStrength);
	Matrix_MulBy(&Camera.TiltM, &tiltY);

	Camera.BobbingHor = (e->Anim.BobbingHor * 0.3f) * e->Anim.BobStrength;
	Camera.BobbingVer = (e->Anim.BobbingVer * 0.6f) * e->Anim.BobStrength;

	/* When standing on the ground, velocity.y is -0.08 (-gravity) */
	/* So add 0.08 to counteract that, so that vel is 0 when standing on ground */
	vel  = 0.08f + Math_Lerp(p->OldVelocity.y, e->Velocity.y, t);
	fall = -vel * 0.05f * p->Tilt.VelTiltStrength / velTiltScale;

	Matrix_RotateX(&velX, fall);
	Matrix_MulBy(&Camera.TiltM, &velX);
	if (!Game_ClassicMode) Camera.TiltPitch = fall;
}


/*########################################################################################################################*
*---------------------------------------------------First person camera---------------------------------------------------*
*#########################################################################################################################*/
static Vec2 FirstPersonCamera_GetOrientation(void) {
	struct LocalPlayer* p = Entities.CurPlayer;
	struct Entity* e = &p->Base;

	Vec2 v;
	v.x = e->Yaw   * MATH_DEG2RAD; 
	v.y = e->Pitch * MATH_DEG2RAD;
	return v;
}

static Vec3 FirstPersonCamera_GetPosition(float t) {
	struct LocalPlayer* p = Entities.CurPlayer;
	struct Entity* e = &p->Base;

	Vec3 camPos   = Entity_GetEyePosition(e);
	float yaw     = e->Yaw * MATH_DEG2RAD;
	PerspectiveCamera_CalcViewBobbing(p, t, 1);
	
	camPos.y += Camera.BobbingVer;
	camPos.x += Camera.BobbingHor * Math_CosF(yaw);
	camPos.z += Camera.BobbingHor * Math_SinF(yaw);
	return camPos;
}

static cc_bool FirstPersonCamera_Zoom(float amount) { return false; }
static struct Camera cam_FirstPerson = {
	false,
	PerspectiveCamera_GetProjection,  PerspectiveCamera_GetView,
	FirstPersonCamera_GetOrientation, FirstPersonCamera_GetPosition,
	PerspectiveCamera_UpdateMouse,    Camera_OnRawMovement,
	Window_EnableRawMouse,            Window_DisableRawMouse,
	PerspectiveCamera_GetPickedBlock, FirstPersonCamera_Zoom,
};


/*########################################################################################################################*
*---------------------------------------------------Third person camera---------------------------------------------------*
*#########################################################################################################################*/
#define DEF_ZOOM 3.0f
static float dist_third = DEF_ZOOM, dist_forward = DEF_ZOOM;

static Vec2 ThirdPersonCamera_GetOrientation(void) {
	struct LocalPlayer* p = Entities.CurPlayer;
	struct Entity* e = &p->Base;

	Vec2 v;	
	v.x = e->Yaw   * MATH_DEG2RAD; 
	v.y = e->Pitch * MATH_DEG2RAD;
	if (cam_isForwardThird) { v.x += MATH_PI; v.y = -v.y; }

	v.x += cam_rotOffset.x * MATH_DEG2RAD; 
	v.y += cam_rotOffset.y * MATH_DEG2RAD;
	return v;
}

static float ThirdPersonCamera_GetZoom(struct LocalPlayer* p) {
	float dist = cam_isForwardThird ? dist_forward : dist_third;
	/* Don't allow zooming out when -fly */
	if (dist > DEF_ZOOM && !LocalPlayer_CheckCanZoom(p)) dist = DEF_ZOOM;
	return dist;
}

static Vec3 ThirdPersonCamera_GetPosition(float t) {
	struct LocalPlayer* p = Entities.CurPlayer;
	struct Entity* e = &p->Base;

	float dist = ThirdPersonCamera_GetZoom(p);
	Vec3 target, dir;
	Vec2 rot;

	PerspectiveCamera_CalcViewBobbing(p, t, dist);
	target = Entity_GetEyePosition(e);
	target.y += Camera.BobbingVer;

	rot = Camera.Active->GetOrientation();
	dir = Vec3_GetDirVector(rot.x, rot.y);
	Vec3_Negate(&dir, &dir);

	Picking_ClipCameraPos(&target, &dir, dist, &cameraClipPos);
	return cameraClipPos.intersect;
}

static cc_bool ThirdPersonCamera_Zoom(float amount) {
	float* dist   = cam_isForwardThird ? &dist_forward : &dist_third;
	float newDist = *dist - amount;

	*dist = max(newDist, 2.0f); 
	return true;
}

static struct Camera cam_ThirdPerson = {
	true,
	PerspectiveCamera_GetProjection,  PerspectiveCamera_GetView,
	ThirdPersonCamera_GetOrientation, ThirdPersonCamera_GetPosition,
	PerspectiveCamera_UpdateMouse,    Camera_OnRawMovement,
	Window_EnableRawMouse,            Window_DisableRawMouse,
	PerspectiveCamera_GetPickedBlock, ThirdPersonCamera_Zoom,
};
static struct Camera cam_ForwardThird = {
	true,
	PerspectiveCamera_GetProjection,  PerspectiveCamera_GetView,
	ThirdPersonCamera_GetOrientation, ThirdPersonCamera_GetPosition,
	PerspectiveCamera_UpdateMouse,    Camera_OnRawMovement,
	Window_EnableRawMouse,            Window_DisableRawMouse,
	PerspectiveCamera_GetPickedBlock, ThirdPersonCamera_Zoom,
};


/*########################################################################################################################*
*-----------------------------------------------------General camera------------------------------------------------------*
*#########################################################################################################################*/
static void OnRawMovement(void* obj, float deltaX, float deltaY) {
	Camera.Active->OnRawMovement(deltaX, deltaY, 0);
}

static void OnAxisUpdate(void* obj, int port, int axis, float x, float y) {
	if (!Input.RawMode) return;
	if (Gamepad_AxisBehaviour[axis] != AXIS_BEHAVIOUR_CAMERA) return;

	Camera.Active->OnRawMovement(x, y, port);
}

static void OnHacksChanged(void* obj) {
	struct HacksComp* h = &Entities.CurPlayer->Hacks;
	/* Leave third person if not allowed anymore */
	if (!h->CanUseThirdPerson || !h->Enabled) Camera_CycleActive();
}
#include "Chat.h"
static cc_bool hackPermMsgs;
void Camera_CycleActive(void) {
	hackPermMsgs = Options_GetBool(OPT_HACK_PERM_MSGS, true);
	struct LocalPlayer* p = &LocalPlayer_Instances[0];
	if (Game_ClassicMode) return;
	Camera.Active = Camera.Active->next;

	if (!p->Hacks.CanUseThirdPerson || !p->Hacks.Enabled) {
		Camera.Active = &cam_FirstPerson;
		if (!p->_warnedCamera) {
			p->_warnedCamera = true;
			if (hackPermMsgs) Chat_AddRaw("&cThirdPerson is currently disabled");
		}
	}
	cam_isForwardThird = Camera.Active == &cam_ForwardThird;

	/* reset rotation offset when changing cameras */
	cam_rotOffset.x = 0.0f; cam_rotOffset.y = 0.0f;
	Camera_UpdateProjection();
}

static struct Camera* cams_head;
static struct Camera* cams_tail;
void Camera_Register(struct Camera* cam) {
	LinkedList_Append(cam, cams_head, cams_tail);
	/* want a circular linked list */
	cam->next = cams_head;
}

static cc_bool cam_focussed;
void Camera_CheckFocus(void) {
	cc_bool focus = Gui.InputGrab == NULL;
	if (focus == cam_focussed) return;
	cam_focussed = focus;

	if (focus) {
		Camera.Active->AcquireFocus();
	} else {
		Camera.Active->LoseFocus();
	}
}

void Camera_SetFov(int fov) {
	if (Camera.Fov == fov) return;
	Camera.Fov = fov;
	Camera_UpdateProjection();
}

void Camera_UpdateProjection(void) {
	Camera.Active->GetProjection(&Gfx.Projection);
	Gfx_LoadMatrix(MATRIX_PROJ,  &Gfx.Projection);
	Event_RaiseVoid(&GfxEvents.ProjectionChanged);
}

static void ZoomScrollReleased(int key, struct InputDevice* device) {
	Camera_SetFov(Camera.DefaultFov);
}

static void OnInit(void) {
	Camera_Register(&cam_FirstPerson);
	Camera_Register(&cam_ThirdPerson);
	Camera_Register(&cam_ForwardThird);
	Bind_OnReleased[BIND_ZOOM_SCROLL] = ZoomScrollReleased;

	Camera.Active = &cam_FirstPerson;
	Event_Register_(&PointerEvents.RawMoved,      NULL, OnRawMovement);
	Event_Register_(&ControllerEvents.AxisUpdate, NULL, OnAxisUpdate);
	Event_Register_(&UserEvents.HackPermsChanged, NULL, OnHacksChanged);

#ifdef CC_BUILD_WIN
	Camera.Sensitivity = Options_GetInt(OPT_SENSITIVITY, 1, 200, 40);
#else
	Camera.Sensitivity = Options_GetInt(OPT_SENSITIVITY, 1, 200, 30);
#endif
	Camera.Clipping    = Options_GetBool(OPT_CAMERA_CLIPPING, true);
	Camera.Invert      = Options_GetBool(OPT_INVERT_MOUSE, false);
	Camera.Mass        = Options_GetFloat(OPT_CAMERA_MASS, 1, 100, 20);
	Camera.Smooth      = Options_GetBool(OPT_CAMERA_SMOOTH, false);

	Camera.DefaultFov  = Options_GetInt(OPT_FIELD_OF_VIEW, 1, 179, 70);
	Camera.Fov         = Camera.DefaultFov;
	Camera.ZoomFov     = Camera.DefaultFov;
	Camera_UpdateProjection();
}

struct IGameComponent Camera_Component = {
	OnInit /* Init  */
};
