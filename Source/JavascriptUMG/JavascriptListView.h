#pragma once

#include "JavascriptTreeView.h"
#include "JavascriptListView.generated.h"

class UJavascriptContext;

/**
* Allows thousands of items to be displayed in a list.  Generates widgets dynamically for each item.
*/
UCLASS(Experimental)
class JAVASCRIPTUMG_API UJavascriptListView : public UJavascriptTreeView
{
	GENERATED_UCLASS_BODY()

public:	
	/** The height of each widget */
	UPROPERTY(EditAnywhere, Category = Content)
	float ItemHeight;

	/** Event fired when a tutorial stage ends */
	UFUNCTION(BlueprintImplementableEvent, Category = "Javascript")
	void OnClick(UObject* Object);

	/** Refreshes the list */
	UFUNCTION(BlueprintCallable, Category = "Javascript")
	void RequestListRefresh();

	//UFUNCTION(BlueprintCallable, Category = "Javascript")
	void GetSelectedItems(TArray<UObject*>& OutItems) override;

	//UFUNCTION(BlueprintCallable, Category = "Javascript")
	void SetSelection(UObject* SoleSelectedItem) override;

	// UWidget interface
	virtual TSharedRef<SWidget> RebuildWidget() override;
	// End of UWidget interface

	TSharedPtr< SListView<UObject*> > MyListView;
};
