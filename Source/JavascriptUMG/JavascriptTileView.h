#pragma once

#include "TileView.h"
#include "JavascriptTileView.generated.h"

class UJavascriptContext;

/**
* Allows thousands of items to be displayed in a Tile.  Generates widgets dynamically for each item.
*/
UCLASS(Experimental)
class JAVASCRIPTUMG_API UJavascriptTileView : public UTileView
{
	GENERATED_UCLASS_BODY()

public:	
	UPROPERTY(BlueprintReadWrite, Category = "Javascript")
	UJavascriptContext* JavascriptContext;

	/** Event fired when a tutorial stage ends */
	UFUNCTION(BlueprintImplementableEvent, Category = "Javascript")
	void OnClick(UObject* Object);

	/** Event fired when a tutorial stage ends */
	UFUNCTION(BlueprintImplementableEvent, Category = "Javascript")
	void OnDoubleClick(UObject* Object);

	/** Event fired when a tutorial stage ends */
	UFUNCTION(BlueprintImplementableEvent, Category = "Javascript")
	void OnSelectionChanged(UObject* Object);	

	// UWidget interface
	//virtual TSharedRef<STableViewBase> RebuildListWidget() override;
	// End of UWidget interface

	// UObject interface
	virtual void ProcessEvent(UFunction* Function, void* Parms) override;
	// End of UObject interface

protected:
	virtual void OnItemClickedInternal(UObject* Item) { OnClick(Item); }
	virtual void OnItemDoubleClickedInternal(UObject* Item) { OnDoubleClick(Item); }
	virtual void OnSelectionChangedInternal(UObject* FirstSelectedItem) { OnSelectionChanged(FirstSelectedItem); }
};
