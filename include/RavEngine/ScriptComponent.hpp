#pragma once
#include "Transform.hpp"
#include "Queryable.hpp"
#include "Entity.hpp"
#include "ComponentWithOwner.hpp"

namespace RavEngine {
	class World;
	/**
	Define an Entity-side Script which can contain code. Subclass to add behavior. Be sure to invoke the base class constructor!
	*/
	class ScriptComponent : public ComponentWithOwner,  public Queryable<ScriptComponent> {
	public:
        
        ScriptComponent(entity_t owner) : ComponentWithOwner(owner){}
        
		/**
		Override to provide cleanup behavior.
		*/
		virtual ~ScriptComponent() {}

		/**
		Invoked when the owning entity of this script has been added to the world.
		*/
		virtual void Start() {};

		/**
		Called by the world when the owning entity has been despawned, but before despawn work has begun. It may not be deallocated immediately after depending on reference counts. However it is best to stop or destroy anything possible.
		*/
		virtual void Stop() {}

		/**
		Invoked as the last step of the systems execution on a background thread. Any cross-object access must be appropriately protected.
		@param fpsScale the frame rate scalar for this frame.
		*/
		virtual void Tick(float fpsScale) = 0;

		/**
		Shortcut to get the transform component of the attached entity
		@throws if the script is not attached to any entity.
		*/
		Transform& GetTransform();
	};
}
