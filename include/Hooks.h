#pragma once

namespace Hooks
{
	/// Installs SE/AE hooks that compact static hkbNode::id assignments to
	/// hkbStateMachine nodes only when a behavior graph exceeds 32,766 nodes.
	bool Install();
}
