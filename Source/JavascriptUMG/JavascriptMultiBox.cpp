
#include "JavascriptMultiBox.h"
#include "SJavascriptBox.h"
#include "SBox.h"
#include "SSpacer.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"

namespace
{
	SBox* Target;
}

void UJavascriptMultiBox::Setup(TSharedRef<SBox> Box)
{	
	Box->SetContent(SNew(SSpacer));

	Target = &(Box.Get());
	OnHook.Execute(FName("Main"),this,FJavascriptMenuBuilder());
	Target = nullptr;
}

void UJavascriptMultiBox::Bind(FJavascriptMenuBuilder Builder)
{
	if (Builder.MultiBox)
	{
		Target->SetContent(Builder.MultiBox->MakeWidget());
	}
}

TSharedRef<SWidget> UJavascriptMultiBox::RebuildWidget()
{	
	auto PrimaryArea = SNew(SJavascriptBox).Widget(this);

	Setup(PrimaryArea);

	return PrimaryArea;
}

void UJavascriptMultiBox::AddPullDownMenu(FJavascriptMenuBuilder& Builder, FName Id, const FText& Label, const FText& ToolTip)
{
	if (Builder.MenuBar)
	{
		Builder.MenuBar->AddPullDownMenu(
			Label,
			ToolTip,
			FNewMenuDelegate::CreateLambda([this, Id](FMenuBuilder& MenuBuilder) {
				FJavascriptMenuBuilder builder;
				builder.MultiBox = builder.Menu = &MenuBuilder;
				this->OnHook.Execute(Id,this, builder);
			})
		);
	}
}

void UJavascriptMultiBox::AddSubMenu(FJavascriptMenuBuilder& Builder, FName Id, const FText& Label, const FText& ToolTip, const bool bInOpenSubMenuOnClick)
{
	if (Builder.Menu)
	{
		Builder.Menu->AddSubMenu(
			Label,
			ToolTip,
			FNewMenuDelegate::CreateLambda([this, Id](FMenuBuilder& MenuBuilder) {
				FJavascriptMenuBuilder builder;
				builder.MultiBox = builder.Menu = &MenuBuilder;
				this->OnHook.Execute(Id, this, builder);
			}),
			bInOpenSubMenuOnClick,
			FSlateIcon()
		);
	}
}
