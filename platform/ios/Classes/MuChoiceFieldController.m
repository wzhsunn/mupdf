//
//  MuChoiceFieldController.m
//  MuPDF
//
//  Copyright (c) 2013 Artifex Software, Inc. All rights reserved.
//

#import "MuChoiceFieldController.h"

@interface MuChoiceFieldController ()

@end

@implementation MuChoiceFieldController

- (id)initWithChoices:(NSArray *)_choices okayAction:(void (^)(NSArray *))block
{
	self = [super initWithNibName:@"MuChoiceFieldController" bundle:nil];
	if (self)
	{
		okayBlock = Block_copy(block);
		choices = [_choices retain];
		selected = -1;
	}
	return self;
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	_picker.dataSource = self;
	_picker.delegate = self;
	// Do any additional setup after loading the view from its nib.
}

- (void)didReceiveMemoryWarning
{
	[super didReceiveMemoryWarning];
	// Dispose of any resources that can be recreated.
}

- (void)dealloc
{
	[okayBlock release];
	[choices release];
	[_picker release];
	[super dealloc];
}

- (NSInteger)numberOfComponentsInPickerView:(UIPickerView *)pickerView
{
	return 1;
}

- (NSInteger)pickerView:(UIPickerView *)pickerView numberOfRowsInComponent:(NSInteger)component
{
	return [choices count];
}

- (NSString *)pickerView:(UIPickerView *)pickerView titleForRow:(NSInteger)row forComponent:(NSInteger)component
{
	return [choices objectAtIndex:row];
}

- (void) pickerView:(UIPickerView *)pickerView didSelectRow:(NSInteger)row inComponent:(NSInteger)component
{
	selected = row;
}

- (IBAction)okayTapped:(id)sender
{
	if (selected > -1)
		okayBlock([NSArray arrayWithObject:[choices objectAtIndex:selected]]);
	[self dismissViewControllerAnimated:YES completion:nil];
}

- (IBAction)cancelTapped:(id)sender
{
	[self dismissViewControllerAnimated:YES completion:nil];
}

@end
